﻿#include "jlcxx/array.hpp"
#include "jlcxx/jlcxx.hpp"
#include "jlcxx/functions.hpp"
#include "jlcxx/jlcxx_config.hpp"

#include <julia.h>
#if JULIA_VERSION_MAJOR == 0 && JULIA_VERSION_MINOR > 4 || JULIA_VERSION_MAJOR > 0
#include <julia_threads.h>
#endif

namespace jlcxx
{

jl_module_t* g_cxxwrap_module;
jl_datatype_t* g_cppfunctioninfo_type;

void (*g_protect_from_gc)(jl_value_t*);
void (*g_unprotect_from_gc)(jl_value_t*);

JLCXX_API void protect_from_gc(jl_value_t* v)
{
  JL_GC_PUSH1(&v);
  g_protect_from_gc(v);
  JL_GC_POP();
}

JLCXX_API void unprotect_from_gc(jl_value_t* v)
{
  g_unprotect_from_gc(v);
}

JLCXX_API std::stack<std::size_t>& gc_free_stack()
{
  static std::stack<std::size_t> m_stack;
  return m_stack;
}

Module::Module(jl_module_t* jmod) :
  m_jl_mod(jmod),
  m_pointer_array((jl_array_t*)jl_get_global(jmod, jl_symbol("__cxxwrap_pointers")))
{
}

cxxint_t Module::store_pointer(void *ptr)
{
  assert(ptr != nullptr);
  m_pointer_array.push_back(ptr);
  return m_pointer_array.size();
}

void Module::bind_constants(jl_module_t* mod)
{
  for(auto& dt_pair : m_jl_constants)
  {
    jl_set_const(mod, jl_symbol(dt_pair.first.c_str()), dt_pair.second);
  }
}

Module &ModuleRegistry::create_module(jl_module_t* jmod)
{
  if(jmod == nullptr)
    throw std::runtime_error("Can't create module from null Julia module");
  if(m_modules.count(jmod))
    throw std::runtime_error("Error registering module: " + module_name(jmod) + " was already registered");

  m_current_module = new Module(jmod);
  m_modules[jmod].reset(m_current_module);
  return *m_current_module;
}

Module& ModuleRegistry::current_module()
{
  assert(m_current_module != nullptr);
  return *m_current_module;
}

void FunctionWrapperBase::set_pointer_indices()
{
  m_pointer_index = m_module->store_pointer(pointer());
  void* thk = thunk();
  if(thk != nullptr)
  {
    m_thunk_index = m_module->store_pointer(thunk());
  }
}

JLCXX_API ModuleRegistry& registry()
{
  static ModuleRegistry m_registry;
  return m_registry;
}

JLCXX_API jl_value_t* julia_type(const std::string& name, const std::string& module_name)
{
  std::vector<jl_module_t*> mods;
  mods.reserve(6);
  jl_module_t* current_mod = registry().has_current_module() ? registry().current_module().julia_module() : nullptr;
  if(!module_name.empty())
  {
    jl_sym_t* modsym = jl_symbol(module_name.c_str());
    jl_module_t* found_mod = nullptr;
    if(current_mod != nullptr)
    {
      found_mod = (jl_module_t*)jl_get_global(current_mod, modsym);
    }
    if(found_mod == nullptr)
    {
      found_mod = (jl_module_t*)jl_get_global(jl_main_module, jl_symbol(module_name.c_str()));
    }
    if(found_mod != nullptr)
    {
      mods.push_back(found_mod);
    }
    else
    {
      throw std::runtime_error("Failed to find module " + module_name);
    }
  }
  else
  {
    if (current_mod != nullptr)
    {
      mods.push_back(current_mod);
    }
    mods.push_back(jl_main_module);
    mods.push_back(jl_base_module);
    mods.push_back(g_cxxwrap_module);
    mods.push_back(jl_top_module);
  }

  std::string found_type = "null";
  for(jl_module_t* mod : mods)
  {
    if(mod == nullptr)
    {
      continue;
    }

    jl_value_t* gval = julia_type(name, mod);
    if(gval != nullptr)
    {
      return gval;
    }
    gval = jl_get_global(mod, jl_symbol(name.c_str()));
    if(gval != nullptr)
    {
      found_type = julia_type_name(jl_typeof(gval));
    }
  }
  std::string errmsg = "Symbol for type " + name + " was not found. A Value of type " + found_type + " was found instead. Searched modules:";
  for(jl_module_t* mod : mods)
  {
    if(mod != nullptr)
    {
      errmsg +=  " " + symbol_name(mod->name);
    }
  }
  throw std::runtime_error(errmsg);
}

JLCXX_API jl_value_t* julia_type(const std::string& name, jl_module_t* mod)
{
  jl_value_t* gval = jl_get_global(mod, jl_symbol(name.c_str()));
  if(gval != nullptr && (jl_is_datatype(gval) || jl_is_unionall(gval)))
  {
    return gval;
  }
  return nullptr;
}

InitHooks& InitHooks::instance()
{
  static InitHooks hooks;
  return hooks;
}

InitHooks::InitHooks()
{
}

void InitHooks::add_hook(const hook_t hook)
{
  m_hooks.push_back(hook);
}

void InitHooks::run_hooks()
{
  for(const hook_t& h : m_hooks)
  {
    h();
  }
}

JLCXX_API jl_value_t* apply_type(jl_value_t* tc, jl_svec_t* params)
{
#if JULIA_VERSION_MAJOR == 0 && JULIA_VERSION_MINOR < 6
  return jl_apply_type(tc, params);
#else
  return jl_apply_type(jl_is_unionall(tc) ? tc : ((jl_datatype_t*)tc)->name->wrapper, jl_svec_data(params), jl_svec_len(params));
#endif
}

// jl_value_t* ConvertToJulia<std::wstring, false, false, false>::operator()(const std::wstring& str) const
// {
//   static const JuliaFunction wstring_to_julia("wstring_to_julia", "CxxWrap");
//   return wstring_to_julia(str.c_str(), static_cast<cxxint_t>(str.size()));
// }

// std::wstring ConvertToCpp<std::wstring, false, false, false>::operator()(jl_value_t* jstr) const
// {
//   static const JuliaFunction wstring_to_cpp("wstring_to_cpp", "CxxWrap");
//   ArrayRef<wchar_t> arr((jl_array_t*)wstring_to_cpp(jstr));
//   return std::wstring(arr.data(), arr.size());
// }

static constexpr const char* dt_prefix = "__cxxwrap_dt_";

jl_datatype_t* existing_datatype(jl_module_t* mod, jl_sym_t* name)
{
  const std::string prefixed_name = dt_prefix + symbol_name(name);
  jl_value_t* found_dt = jl_get_global(mod, jl_symbol(prefixed_name.c_str()));
  if(found_dt == nullptr || !jl_is_datatype(found_dt))
  {
    return nullptr;
  }
  return (jl_datatype_t*)found_dt;
}

void set_internal_constant(jl_module_t* mod, jl_datatype_t* dt, const std::string& prefixed_name)
{
  jl_set_const(mod, jl_symbol(prefixed_name.c_str()), (jl_value_t*)dt);
}

JLCXX_API jl_datatype_t* new_datatype(jl_sym_t *name,
                            jl_module_t* module,
                            jl_datatype_t *super,
                            jl_svec_t *parameters,
                            jl_svec_t *fnames, jl_svec_t *ftypes,
                            int abstract, int mutabl,
                            int ninitialized)
{
  if(module == nullptr)
  {
    throw std::runtime_error("null module when creating type");
  }
  jl_datatype_t* dt = existing_datatype(module, name);
  if(dt != nullptr)
  {
    return dt;
  }

  // std::stringstream dt_def;
  // if(mutabl)
  // {
  //   dt_def << "mutable ";
  // }
  // dt_def << "struct " << symbol_name(name);
  // const size_t nparams = jl_svec_len(parameters);
  // if(nparams != 0)
  // {
  //   dt_def << "{";
  //   for(size_t i = 0; i != nparams; ++i)
  //   {
  //     dt_def << julia_type_name(jl_svecref(parameters,i)) << ",";
  //   }
  //   dt_def << "}";
  // }

  // dt_def << " <: " << julia_type_name(super);

  // std::cout << "adding type " << dt_def.str() << std::endl;

  dt = jl_new_datatype(name, module, super, parameters, fnames, ftypes, abstract, mutabl, ninitialized);
  set_internal_constant(module, dt, dt_prefix + symbol_name(name));
  return dt;
}

JLCXX_API jl_datatype_t* new_bitstype(jl_sym_t *name,
                            jl_module_t* module,
                            jl_datatype_t *super,
                            jl_svec_t *parameters, const size_t nbits)
{
  assert(module != nullptr);
  jl_datatype_t* dt = existing_datatype(module, name);
  if(dt != nullptr)
  {
    return dt;
  }

  dt = jl_new_primitivetype((jl_value_t*)name, module, super, parameters, nbits);
  set_internal_constant(module, dt, dt_prefix + symbol_name(name));
  return dt;
}

namespace detail
{
  template<typename T>
  struct AddIntegerTypes
  {
  };

  template<typename T, typename... OtherTypesT>
  struct AddIntegerTypes<ParameterList<T, OtherTypesT...>>
  {
    void operator()(const std::string& basename, const std::string& prefix)
    {
      if(has_julia_type<T>())
      {
        return;
      }
      std::stringstream tname;
      tname << prefix << (std::is_unsigned<T>::value ? "U" : "") << basename;
      tname << sizeof(T)*8;
      jl_module_t* mod = prefix.empty() ? jl_base_module : g_cxxwrap_module;
      set_julia_type<T>((jl_datatype_t*)julia_type(tname.str(), mod));
      AddIntegerTypes<ParameterList<OtherTypesT...>>()(basename, prefix);
    }
  };

  template<>
  struct AddIntegerTypes<ParameterList<>>
  {
    void operator()(const std::string&, const std::string&)
    {
    }
  };
}

JLCXX_API void register_core_types()
{
  set_julia_type<void>(jl_void_type);
  set_julia_type<float>(jl_float32_type);
  set_julia_type<double>(jl_float64_type);
  set_julia_type<bool>((jl_datatype_t*)julia_type("CxxBool", g_cxxwrap_module));
  set_julia_type<char>((jl_datatype_t*)julia_type("CxxChar", g_cxxwrap_module));
  set_julia_type<unsigned char>((jl_datatype_t*)julia_type("CxxUChar", g_cxxwrap_module));
  set_julia_type<wchar_t>((jl_datatype_t*)julia_type("CxxWchar", g_cxxwrap_module));
  
  jlcxx::detail::AddIntegerTypes<fundamental_int_types>()("Int", "");
  jlcxx::detail::AddIntegerTypes<fixed_int_types>()("Int", "Cxx");
  set_julia_type<long>((jl_datatype_t*)julia_type("CxxLong", g_cxxwrap_module));
  set_julia_type<unsigned long>((jl_datatype_t*)julia_type("CxxULong", g_cxxwrap_module));

  set_julia_type<ObjectIdDict>((jl_datatype_t*)julia_type("IdDict", jl_base_module));
}

}
