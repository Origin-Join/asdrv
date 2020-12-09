#include <Windows.h>
#include "lua.hpp"

#include "log.h"
#include "lua_ext_functions.h"

constexpr unsigned char internal_script[] = {
#ifdef _M_IX86
#include "internal_script_x86.h"
#elif _M_X64
#include "internal_script_x64.h"
#endif // _M_IX86
};

// д��ô���������뱣������Կ��Ȼ���ѡ�#109
constexpr ULONGLONG CVal(const int S) {
#define SHIFT_LEFT(index, n)   ((ULONGLONG)internal_script[(index)] << (n * 8))
  return 0ULL | SHIFT_LEFT(__TIMESTAMP__[S], 0)
    | SHIFT_LEFT(__TIMESTAMP__[S + 2], 1)
    | SHIFT_LEFT(S * S, 2)
    | SHIFT_LEFT(_MSC_FULL_VER % sizeof(internal_script), 3)
    | SHIFT_LEFT(S * 13, 4)
    | SHIFT_LEFT(1023, 5)
    | SHIFT_LEFT(__TIME__[S - 3], 6)
    | SHIFT_LEFT(_MSC_FULL_VER % 1023, 7);
}

int internal_lua_script_decrypt(lua_State* L) {
  constexpr ULONGLONG k1mask = CVal(7);
  constexpr ULONGLONG k2mask = CVal(11);

  constexpr ULONGLONG k1 = 0xfb231bf0fa9573f8 ^ k1mask;
  constexpr ULONGLONG k2 = 0x399c230a6839a556 ^ k2mask;

  char pass[16];
  PULONGLONG k = (PULONGLONG)pass;
  k[1] = k2;
  lua_pushlstring(L, (char*)k, 8);
  k[0] = k1;
  lua_pushlstring(L, (char*)k, 8);

  ((PULONGLONG)pass)[0] ^= k1mask;
  lua_pushlstring(L, pass, 8);
  ZeroMemory(pass, 8);

  ((PULONGLONG)pass)[1] ^= k2mask;
  lua_pushlstring(L, pass + 8, 8);
  ZeroMemory(pass + 8, 8);
  lua_concat(L, 2);

  lua_insert(L, 1);
  int n = lua_crypt_decrypt(L);
  if (n == 1) {
    size_t len = 0;
    const char* script = lua_tolstring(L, -1, &len);
    lua_pushlstring(L, script + 32, len - 32); 
  }
  return n;
}

// local func = load(ext.de(internal_script));
// local f = func(index);
// f(internals, ...)
// ����һ��������һ��internals��������Ҫ�����߼���push��������call��
// ����ֵ��ʾ�������������������internal_script.lua��main���õĹ��ܴ���Ļ�����ʽ��func(internals, ...)
int load_internal_lua_script(lua_State* L, int index) {
  Log("internal initialize script");
  lua_initialize_script(L, internal_lua_script_decrypt, (char*)internal_script, sizeof(internal_script));

  Log("internal real function");
  lua_pushinteger(L, index);
  LUA_CALL(L, 1, 1, 0);

  Log("internal initialize ext api");
  lua_initialize_ext(L, InternalApi, ARRAYSIZE(InternalApi), 0);
  return 1;
}
