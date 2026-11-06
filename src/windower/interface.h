#pragma once

// Windower 4's native plugin interface, version 0x40004, recovered from Hook.dll 4.4.2.0 (2024-10-28) and the plugins
// shipped with it. Windower publishes no SDK; docs/windower-interface.md holds the trace, with addresses, for every slot.
//
// Each slot names its evidence and a confidence:
//   high   - the call site in Hook.dll and a plugin's implementation agree, or a mangled name gives the signature
//   medium - the call site or the implementation alone
//   low    - argument count only; name and meaning unknown
// "(live)" marks what only a live client can settle.
//
// Every method is __stdcall with `this` on the stack, COM style: Hook.dll's callers push the object, and the plugins'
// implementations end in `ret 4 * (args + 1)`. The structs hold no data: only their vtables are used.

#include <cstdint>

namespace memoria::windower
{

// GetInterfaceVersion's answer. Hook.dll 0x1005434b loads a plugin only on an exact match: a higher number is
// "requires a newer version of Windower", a lower one "This plugin is out of date".
inline constexpr uint32_t INTERFACE_VERSION = 0x40004;

// IWindower::GetVersion's answer on the same build: 0x40004 as a double (Hook.dll 0x10145ae8).
inline constexpr double HOST_VERSION = 262148.0;

// IPlugin::GetInfo fills this. Hook.dll copies 0x328 bytes (0x10054473, rep movsd 0xca dwords) into the plugin's
// entry, prints the author, and splits `commands` on '|' for the console's command lookup (0x10054f50). The rest is
// never read by the code traced; it is left zero.
struct PluginInfo
{
    char unknown0[0x100];
    char author[0x100]; // high: "Plugin: %s (author: %s, ...)" at 0x100544bc reads info + 0x100
    char unknown200[0x108];
    char commands[0x20]; // high: '|'-separated keywords, matched case-insensitively against a command's first word
};
static_assert(sizeof(PluginInfo) == 0x328);

// Windower's console. Hook.dll class Console, vtable 0x1013bfa4.
struct IConsole
{
    virtual auto __stdcall unknown0() -> void             = 0;
    virtual auto __stdcall unknown1(const char*) -> void  = 0;
    virtual auto __stdcall unknown2() -> void             = 0;
    virtual auto __stdcall unknown3(void*, void*) -> void = 0;
    // high: Hook.dll prints "FFXI Windower" and its credits through it (0x10038856); ret 8.
    virtual auto __stdcall Write(const char* line) -> void = 0;
    virtual auto __stdcall unknown5() -> void              = 0;
    // medium: runs a console command (handles alias and bind); LuaCore's windower.send_command calls it as (0, text, 0).
    virtual auto __stdcall RunCommand(int source, const char* line, int flags) -> void = 0;
    // low: runs scripts/<name>.txt ("exec ").
    virtual auto __stdcall unknown7(void*, void*) -> void = 0;
};

// The game. Hook.dll class FFXI, vtable 0x1013af2c.
struct IFFXI
{
    virtual auto __stdcall unknown0(void*, void*) -> void = 0;
    // high: Hook.dll lists binds with (text, 0x8d); LuaCore's windower.add_to_chat queue drains to it as (text, mode).
    virtual auto __stdcall AddToChat(const char* text, int mode) -> void = 0;
    // medium: hands outgoing text to the plugins' OutgoingText (0x10031e10).
    virtual auto __stdcall unknown2(void*, void*) -> void = 0;
};

// The host each plugin gets in Attach. Hook.dll class PluginManager, vtable 0x1014259c; all but slot 3 read Hook.dll's
// global state object (0x1018667c).
struct IWindower
{
    // high: returns HOST_VERSION (fld 0x10145ae8).
    virtual auto __stdcall GetVersion() -> double = 0;
    // medium: copies 0x4164 bytes of settings into out and returns it; WindowerPlugin builds
    // "<out + 0x2130>\plugins\settings\" from it, so +0x2130 holds Windower's folder.
    virtual auto __stdcall GetSettings(void* out) -> void* = 0;
    // high: the game window (global + 8, used with ShowWindow and SetWindowPos).
    virtual auto __stdcall GetWindow() -> void* = 0;
    // low: returns this + 0x44c08.
    virtual auto __stdcall unknown3() -> void* = 0;
    // low: no arguments; thread-local static initialisation.
    virtual auto __stdcall unknown4() -> void* = 0;
    // high: global + 0x10, the console; WindowerPlugin keeps it at plugin + 0xc, LuaCore's send_command uses it.
    virtual auto __stdcall GetConsole() -> IConsole* = 0;
    // high (type): global + 0x4180, a TextHandler (vtable 0x1014275c).
    virtual auto __stdcall GetTextHandler() -> void* = 0;
    // high (type): global + 0x4184, a PrimitiveHandler.
    virtual auto __stdcall GetPrimitiveHandler() -> void* = 0;
    // high (type): global + 0x6a80, a PacketStreamHandler (vtable 0x101421b8).
    virtual auto __stdcall GetPacketStreamHandler() -> void* = 0;
    // low: returns null in this build.
    virtual auto __stdcall unknown9() -> void* = 0;
    // high: global + 0x23fb0, the FFXI object; LuaCore's add_to_chat drain calls it then AddToChat.
    virtual auto __stdcall GetFFXI() -> IFFXI* = 0;
};

// What a plugin's CreateInstance returns. Plugins derive WindowerPlugin -> PluginBase -> IPlugin (RTTI in every
// shipped plugin); PluginBase's vtable has 22 slots: these 21 and its deleting destructor, which Hook.dll never calls.
struct IPlugin
{
    // high: Hook.dll calls it once, right after GetInfo (0x100544f5), with the PluginManager. Plugins start here.
    virtual auto __stdcall Attach(IWindower* host) -> void = 0;
    // high: unload, before Release (0x10054d50); the entry is already marked disabled, so no more events arrive.
    virtual auto __stdcall Unload() -> void = 0;
    // high: PluginBase deletes the plugin; Hook.dll then calls FreeLibrary.
    virtual auto __stdcall Release() -> void = 0;
    // high: Hook.dll passes an uninitialised 0x328-byte buffer and copies what the return points at.
    virtual auto __stdcall GetInfo(PluginInfo* out) -> PluginInfo* = 0;
    // high: a console line whose first word matches one of info.commands (0x1005511a): the rest of the line, then
    // the keyword. LuaCore's Callback_HandleCommand is (char*, char*).
    virtual auto __stdcall HandleCommand(const char* args, const char* keyword) -> void = 0;
    // medium: a line no plugin keyword and no built-in command took, offered to each plugin in turn (0x1003c7f1);
    // true stops the search. LuaCore raises "unhandled command" from it.
    virtual auto __stdcall HandleUnhandledCommand(const char* line) -> bool = 0;
    // medium: from the Direct3D device's Reset (0x100428e0), before and after; which is which is (live).
    virtual auto __stdcall DeviceReset1() -> void = 0;
    virtual auto __stdcall DeviceReset2() -> void = 0;
    // high: from IDirect3DDevice8::BeginScene (0x10042990); LuaCore raises "prerender" here.
    virtual auto __stdcall Prerender() -> void = 0;
    // high: from EndScene (0x10042a10); LuaCore raises "postrender".
    virtual auto __stdcall Postrender() -> void = 0;
    // high: LuaCore's Callback_IncomingText(const char*, char*, int&), "incoming text".
    virtual auto __stdcall IncomingText(const char* original, char* modified, int* mode) -> void = 0;
    // high: LuaCore's Callback_OutgoingText(const char*, char*, ID::CommandSource), "outgoing text".
    virtual auto __stdcall OutgoingText(const char* original, char* modified, int source) -> void = 0;
    // high: one incoming game packet (0x100551a0). original is Hook.dll's copy taken before any plugin ran; modified
    // is the packet in the chunk, which plugins may change; blocked is true if an earlier plugin blocked it. Return
    // true to block. The size is the header's: (modified[1] & 0xfe) * 2. Plugins run in load order.
    virtual auto __stdcall IncomingChunk(const uint8_t* original, uint8_t* modified, bool injected, bool blocked) -> bool = 0;
    // high: the same for an outgoing packet.
    virtual auto __stdcall OutgoingChunk(const uint8_t* original, uint8_t* modified, bool injected, bool blocked) -> bool = 0;
    // low: one argument; LuaCore leaves it empty.
    virtual auto __stdcall unknown14(void*) -> void = 0;
    // medium: asked by "unload all"; true keeps the plugin loaded.
    virtual auto __stdcall KeepOnUnloadAll() -> bool = 0;
    // low: no arguments; WindowerPlugin clears its cached player state.
    virtual auto __stdcall unknown16() -> void = 0;
    // high: LuaCore's HandleMouse is __stdcall bool(int, int, int, int, bool), "mouse".
    virtual auto __stdcall Mouse(int type, int x, int y, int delta, bool blocked) -> bool = 0;
    // high: LuaCore's HandleKeyboard is __stdcall bool(int, char, bool), "keyboard".
    virtual auto __stdcall Keyboard(int dik, char pressed, bool blocked) -> bool = 0;
    // medium: item events from the packet parser (0x10055de0); LuaCore raises "remove item" from slot 20, so 19 is
    // likely "add item". The first argument is a byte.
    virtual auto __stdcall AddItem(uint8_t bag, int index, int id, int count) -> void    = 0;
    virtual auto __stdcall RemoveItem(uint8_t bag, int index, int id, int count) -> void = 0;
};

} // namespace memoria::windower

// The two exports Hook.dll resolves by name (0x10054335, 0x10054380). Both take no arguments; their calling
// convention does not matter.
extern "C" auto GetInterfaceVersion() -> uint32_t;
extern "C" auto CreateInstance() -> memoria::windower::IPlugin*;
