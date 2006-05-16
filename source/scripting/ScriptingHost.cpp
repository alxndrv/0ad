#include "precompiled.h"

#include <sstream>

#include "ScriptingHost.h"
#include "ScriptGlue.h"
#include "CConsole.h"
#include "Profile.h"
#include "ps/CLogger.h"

#include "lib/res/res.h"


#if OS_WIN
#ifdef NDEBUG
# pragma comment (lib, "js32.lib")
#else
# pragma comment (lib, "js32d.lib")
#endif
#endif

#define LOG_CATEGORY "scriptinghost"

extern CConsole* g_Console;

namespace
{
	const int RUNTIME_MEMORY_ALLOWANCE = 16 * 1024 * 1024;
	const int STACK_CHUNK_SIZE = 16 * 1024;

	JSClass GlobalClass = 
	{
		"global", 0,
		JS_PropertyStub, JS_PropertyStub,
		JS_PropertyStub, JS_PropertyStub,
		JS_EnumerateStub, JS_ResolveStub,
		JS_ConvertStub, JS_FinalizeStub
	};
}

ScriptingHost::ScriptingHost()
	: m_RunTime(NULL), m_Context(NULL), m_GlobalObject(NULL)
{
    m_RunTime = JS_NewRuntime(RUNTIME_MEMORY_ALLOWANCE);
	if(!m_RunTime)
		throw PSERROR_Scripting_SetupFailed();

    m_Context = JS_NewContext(m_RunTime, STACK_CHUNK_SIZE);
	if(!m_Context)
		throw PSERROR_Scripting_SetupFailed();

	JS_SetErrorReporter(m_Context, ScriptingHost::ErrorReporter);

	m_GlobalObject = JS_NewObject(m_Context, &GlobalClass, NULL, NULL);
	if(!m_GlobalObject)
		throw PSERROR_Scripting_SetupFailed();

#ifndef NDEBUG
	// Register our script and function handlers - note: docs say they don't like
	// nulls passed as closures, nor should they return nulls.
	JS_SetExecuteHook( m_RunTime, jshook_script, this );
	JS_SetCallHook( m_RunTime, jshook_function, this );
#endif

	if (JS_InitStandardClasses(m_Context, m_GlobalObject) == JSVAL_FALSE)
		throw PSERROR_Scripting_SetupFailed();

	if (JS_DefineFunctions(m_Context, m_GlobalObject, ScriptFunctionTable) == JS_FALSE)
		throw PSERROR_Scripting_SetupFailed();

	if( JS_DefineProperties( m_Context, m_GlobalObject, ScriptGlobalTable ) == JS_FALSE )
		throw PSERROR_Scripting_SetupFailed();
}

ScriptingHost::~ScriptingHost()
{
	if (m_Context != NULL)
	{
 		JS_DestroyContext(m_Context);
		m_Context = NULL;
	}

	if (m_RunTime != NULL)
	{
		JS_DestroyRuntime(m_RunTime);
		m_RunTime = NULL;
	}

	JS_ShutDown();
}

// filename, line and globalObject default to 0 (in which case we execute
// the whole script / use our m_GlobalObject)
void ScriptingHost::RunMemScript(const char* script, size_t size, const char* filename, int line, JSObject* globalObject)
{
	if(!filename)
		filename = "unspecified file";
	if(!globalObject)
		globalObject = m_GlobalObject;

	// Maybe TODO: support Unicode input formats?

	jsval rval;
	JSBool ok = JS_EvaluateScript(m_Context, globalObject, script,
		(uint)size, filename, line, &rval); 

	if (ok == JS_FALSE)
		throw PSERROR_Scripting_LoadFile_EvalErrors();
}

// globalObject defaults to 0 (in which case we use our m_GlobalObject).
void ScriptingHost::RunScript(const CStr& filename, JSObject* globalObject)
{
	const char* fn = filename.c_str();

	FileIOBuf buf;
	size_t size;
	if(vfs_load(fn, buf, size) != ERR_OK)	// ERRTODO: translate/pass it on
		throw PSERROR_Scripting_LoadFile_OpenFailed();

	const char* script = (const char*)buf;
	RunMemScript(script, size, fn, 1, globalObject);

	(void)file_buf_free(buf);
}

jsval ScriptingHost::CallFunction(const std::string & functionName, jsval * params, int numParams)
{
	jsval result;

	JSBool ok = JS_CallFunctionName(m_Context, m_GlobalObject, functionName.c_str(), numParams, params, &result);

	if (ok == JS_FALSE)
		throw PSERROR_Scripting_CallFunctionFailed();

	return result;
}

jsval ScriptingHost::ExecuteScript(const CStrW& script, const CStrW& calledFrom, JSObject* contextObject )
{
	jsval rval; 

	/* Unicode->ASCII conversion (mostly) for calledFrom */

	size_t len = wcstombs( NULL, calledFrom, 0 );
	debug_assert( len != (size_t)-1 );
	char* asciiName = new char[len + 1];
	wcstombs( asciiName, calledFrom, len + 1 );

	JSBool ok = JS_EvaluateUCScript(m_Context, contextObject ? contextObject : m_GlobalObject, script.utf16().c_str(), (int)script.Length(), asciiName, 1, &rval); 

	delete[]( asciiName );

	if (!ok) return JSVAL_NULL;

	return rval;
}

// unused
void ScriptingHost::RegisterFunction(const std::string & functionName, JSNative function, int numArgs)
{
	JSFunction * func = JS_DefineFunction(m_Context, m_GlobalObject, functionName.c_str(), function, numArgs, 0);

	if (func == NULL)
		throw PSERROR_Scripting_RegisterFunctionFailed();
}

void ScriptingHost::DefineConstant(const std::string & name, int value)
{
	// First remove this constant if it already exists
	JS_DeleteProperty(m_Context, m_GlobalObject, name.c_str());

	JSBool ok = JS_DefineProperty(	m_Context, m_GlobalObject, name.c_str(), INT_TO_JSVAL(value), 
									NULL, NULL, JSPROP_READONLY);

	if (ok == JS_FALSE)
		throw PSERROR_Scripting_DefineConstantFailed();
}

void ScriptingHost::DefineConstant(const std::string & name, double value)
{
	// First remove this constant if it already exists
	JS_DeleteProperty(m_Context, m_GlobalObject, name.c_str());

	struct JSConstDoubleSpec spec[2];

	spec[0].name = name.c_str();
	spec[0].dval = value;
	spec[0].flags = JSPROP_READONLY;

	spec[1].name = 0;
	spec[1].dval = 0.0;
	spec[1].flags = 0;

	JSBool ok = JS_DefineConstDoubles(m_Context, m_GlobalObject, spec);

	if (ok == JS_FALSE)
		throw PSERROR_Scripting_DefineConstantFailed();
}

void ScriptingHost::DefineCustomObjectType(JSClass *clasp, JSNative constructor, uintN minArgs, JSPropertySpec *ps, JSFunctionSpec *fs, JSPropertySpec *static_ps, JSFunctionSpec *static_fs)
{
	std::string typeName = clasp->name;

	if (m_CustomObjectTypes.find(typeName) != m_CustomObjectTypes.end())
	{
		// This type already exists
		throw PSERROR_Scripting_DefineType_AlreadyExists();
	}

	JSObject * obj = JS_InitClass(	m_Context, m_GlobalObject, 0, 
									clasp,
									constructor, minArgs,				// Constructor, min args
									ps, fs,								// Properties, methods
									static_ps, static_fs);				// Constructor properties, methods

	if (obj != NULL)
	{
		CustomType type;
		
		type.m_Object = obj;
		type.m_Class = clasp;

		m_CustomObjectTypes[typeName] = type;
	}
	else
	{
		throw PSERROR_Scripting_DefineType_CreationFailed();
	}
}

JSObject * ScriptingHost::CreateCustomObject(const std::string & typeName)
{
	std::map < std::string, CustomType > ::iterator it = m_CustomObjectTypes.find(typeName);

	if (it == m_CustomObjectTypes.end())
		throw PSERROR_Scripting_TypeDoesNotExist();

	return JS_ConstructObject(m_Context, (*it).second.m_Class, (*it).second.m_Object, NULL);

}



void ScriptingHost::SetObjectProperty(JSObject * object, const std::string & propertyName, jsval value)
{
	JS_SetProperty(m_Context, object, propertyName.c_str(), &value);
}

jsval ScriptingHost::GetObjectProperty( JSObject* object, const std::string& propertyName )
{
	jsval vp;
	JS_GetProperty( m_Context, object, propertyName.c_str(), &vp );
	return( vp );
}



void ScriptingHost::SetObjectProperty_Double(JSObject* object, const char* propertyName, double value)
{
	jsdouble* d = JS_NewDouble(m_Context, value);
	if (! d)
		throw PSERROR_Scripting_ConversionFailed();

	jsval v = DOUBLE_TO_JSVAL(d);

	if (! JS_SetProperty(m_Context, object, propertyName, &v))
		throw PSERROR_Scripting_ConversionFailed();
}

double ScriptingHost::GetObjectProperty_Double(JSObject* object, const char* propertyName)
{
	jsval v;
	double d;

	if (! JS_GetProperty(m_Context, object, propertyName, &v))
		throw PSERROR_Scripting_ConversionFailed();
	if (! JS_ValueToNumber(m_Context, v, &d))
		throw PSERROR_Scripting_ConversionFailed();
	return d;
}



void ScriptingHost::SetGlobal(const std::string &globalName, jsval value)
{
	JS_SetProperty(m_Context, m_GlobalObject, globalName.c_str(), &value);
}

// unused
jsval ScriptingHost::GetGlobal(const std::string &globalName)
{
	jsval vp;
	JS_GetProperty(m_Context, m_GlobalObject, globalName.c_str(), &vp);
	return vp;
}









//----------------------------------------------------------------------------
// conversions
//----------------------------------------------------------------------------
/*
// These have been removed in favour of ToPrimitive<int>(value)

int ScriptingHost::ValueToInt(const jsval value)
{
	int32 i = 0;

	JSBool ok = JS_ValueToInt32(m_Context, value, &i);

	if (!ok)
		throw PSERROR_Scripting_ConversionFailed();

	return i;
}

bool ScriptingHost::ValueToBool(const jsval value)
{
	JSBool b;

	JSBool ok = JS_ValueToBoolean(m_Context, value, &b);

	if (!ok)
		throw PSERROR_Scripting_ConversionFailed();

	return b == JS_TRUE;
}


double ScriptingHost::ValueToDouble(const jsval value)
{
	jsdouble d;

	JSBool ok = JS_ValueToNumber(m_Context, value, &d);

	if (ok == JS_FALSE || !finite(d))
		throw PSERROR_Scripting_ConversionFailed();

	return d;
}


*/
std::string ScriptingHost::ValueToString(const jsval value)
{
	JSString* string = JS_ValueToString(m_Context, value);
	if (string == NULL)
		throw PSERROR_Scripting_ConversionFailed();

	return std::string(JS_GetStringBytes(string), JS_GetStringLength(string));
}

CStrW ScriptingHost::ValueToUCString( const jsval value )
{
	return CStrW(ValueToUTF16(value));
}

jsval ScriptingHost::UCStringToValue( const CStrW &str )
{
	utf16string utf16=str.utf16();
	return UTF16ToValue(utf16);
}



utf16string ScriptingHost::ValueToUTF16( const jsval value )
{
	JSString* string = JS_ValueToString(m_Context, value);
	if (string == NULL)
		throw PSERROR_Scripting_ConversionFailed();

	jschar *strptr=JS_GetStringChars(string);
	size_t length=JS_GetStringLength(string);
	return utf16string(strptr, strptr+length);
}

jsval ScriptingHost::UTF16ToValue(const utf16string &str)
{
	return STRING_TO_JSVAL(JS_NewUCStringCopyZ(m_Context, str.c_str()));
}

//----------------------------------------------------------------------------










// called by SpiderMonkey whenever someone does JS_ReportError.
// prints that message as well as locus to log, debug output and console.
void ScriptingHost::ErrorReporter(JSContext* UNUSED(cx), const char* message, JSErrorReport* report)
{
	const char* file = report->filename;
	const int line   = report->lineno;
	// apparently there is no further information in this struct we can use
	// because linebuf/tokenptr require a buffer to have been allocated.
	// that doesn't look possible since we are a callback and there is
	// no mention in the dox about where this would happen (typical).

	if(!file)
		file = "(current document)";
	if(!message)
		message = "No error message available";

	// for developer convenience: write to output window so they can
	// doubleclick on that line and be taken to the error locus.
	debug_printf("%s(%d): %s\n", file, line, message);

	// note: CLogger's LOG already takes care of writing to the console,
	// so don't do that here.

	LOG(ERROR, LOG_CATEGORY, "JavaScript Error (%s, line %d): %s", file, line, message);
}

#ifndef NDEBUG

void* ScriptingHost::jshook_script( JSContext* UNUSED(cx), JSStackFrame* UNUSED(fp),
	JSBool before, JSBool* UNUSED(ok), void* closure )
{
	if( before )
	{
		g_Profiler.StartScript( "script invocation" );
	}
	else
		g_Profiler.Stop();

	return( closure );
}

void* ScriptingHost::jshook_function( JSContext* cx, JSStackFrame* fp, JSBool before, JSBool* UNUSED(ok), void* closure )
{
	JSFunction* fn = JS_GetFrameFunction( cx, fp );
	if( before )
	{
		if( fn )
		{
			g_Profiler.StartScript( JS_GetFunctionName( fn ) );
		}
		else
			g_Profiler.StartScript( "function invokation" );
	}
	else
		g_Profiler.Stop();

	return( closure );
}

#endif
