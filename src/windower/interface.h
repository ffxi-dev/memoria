#pragma once

// Windower 4's native plugin interface, version 0x4070300, recovered from Hook.dll 4.7.9.3 and LuaCore 2.6.8.4, the
// plugin shipped with it. Windower publishes no SDK; docs/windower-interface.md holds the trace, with addresses, for
// every slot. Hook.dll 4.4.2.0's interface (0x40004) had other slots and is not supported.
//
// Each slot names its evidence and a confidence:
//   high   - the call site in Hook.dll and LuaCore's implementation agree, or a mangled name gives the signature
//   medium - the call site or the implementation alone
//   low    - argument count only; name and meaning unknown
// "(live)" marks what only a live client can settle.
//
// Every method is __stdcall with `this` on the stack, COM style: Hook.dll's callers push the object, and the
// implementations end in `ret 4 * (args + 1)`. The structs hold no data: only their vtables are used.

#include <cstdint>

namespace memoria::windower
{

// GetInterfaceVersion's answer. Hook.dll 0x1006f322 loads a plugin only on an exact match: a higher number is
// "requires a newer version of Windower", a lower one "This plugin is out of date".
inline constexpr uint32_t INTERFACE_VERSION = 0x4070300;

// Windower's console. Hook.dll class Console, vtable 0x10184bbc.
struct IConsole
{
    // medium: stores the flag at +0x3e4 and the time it changed.
    virtual auto __stdcall SetVisible(bool visible) -> void = 0;
    // medium: reads +0x3e4.
    virtual auto __stdcall IsVisible() -> bool = 0;
    // medium: stores two floats and moves the console's window.
    virtual auto __stdcall SetPosition(float x, float y) -> void = 0;
    // high: adds one line at level 0 (0x1003e7f0 -> 0x1003e8b0, the function Hook.dll prints "Plugin: {}" with).
    virtual auto __stdcall Write(const char* line) -> void = 0;
    // low: no arguments; resets the line buffer.
    virtual auto __stdcall unknown4() -> void = 0;
    // high: runs a console command; LuaCore's windower.send_command calls it as (text, 0).
    virtual auto __stdcall RunCommand(const char* line, int source) -> void = 0;
};

// The game. Hook.dll class ffxi, vtable 0x10178a38.
struct IFFXI
{
    // high: queues the line under a lock for the game to print (0x10036690); LuaCore's windower.add_to_chat calls it
    // as (text, mode).
    virtual auto __stdcall AddToChat(const char* text, int mode) -> void = 0;
};

// The host each plugin gets in Attach. Hook.dll class PluginManager, vtable 0x10185d70; the global slots read Hook.dll's
// state object (pointer at 0x101cdf64). Slot 8, its destructor, is never called by a plugin.
struct IWindower
{
    // high: state + 0x10, Windower's settings; LuaCore builds "<+0x2150>\plugins\" from it.
    virtual auto __stdcall GetSettings() -> const void* = 0;
    // low: state + 4.
    virtual auto __stdcall unknown1() -> void* = 0;
    // low: this + 0xa14b4.
    virtual auto __stdcall unknown2() -> void* = 0;
    // high: state + 8, the console; WindowerPlugin keeps it at plugin + 8, LuaCore's send_command uses it.
    virtual auto __stdcall GetConsole() -> IConsole* = 0;
    // medium (type): state + 0x3190, a TextHandler.
    virtual auto __stdcall GetTextHandler() -> void* = 0;
    // high (type): state + 0x3194, a PrimitiveHandler (vtable 0x10185e1c, stored at 0x1005ed9d).
    virtual auto __stdcall GetPrimitiveHandler() -> void* = 0;
    // high (type): this + 8, a PacketStreamHandler (vtable 0x10185a68).
    virtual auto __stdcall GetPacketStreamHandler() -> void* = 0;
    // high: a static ffxi object; LuaCore's add_to_chat calls it, then AddToChat.
    virtual auto __stdcall GetFFXI() -> IFFXI* = 0;
};

// What a plugin's CreateInstance returns. LuaCore derives WindowerPlugin -> IPlugin (RTTI); WindowerPlugin's first 17
// slots are these, each forwarding to a __thiscall virtual of its own.
struct IPlugin
{
    // high: Hook.dll calls it right after CreateInstance (0x1006f709) and keeps the string; LuaCore returns
    // "Windower Team".
    virtual auto __stdcall GetAuthor() -> const char* = 0;
    // high: '|'-separated keywords (0x1006f77d), matched against a command's first word; LuaCore returns "lua".
    virtual auto __stdcall GetCommands() -> const char* = 0;
    // high: called once, after the two above (0x1006f9b4), with the PluginManager. Plugins start here.
    virtual auto __stdcall Attach(IWindower* host) -> void = 0;
    // high: unload; the plugin deletes itself, then Hook.dll calls FreeLibrary (0x1005f386). LuaCore stops its
    // addons, then calls its deleting destructor.
    virtual auto __stdcall Unload() -> void = 0;
    // high: true refuses "unload" with "Core plugins cannot be manually unloaded" (0x1007064b) and keeps the plugin
    // through "unload all" (0x10070c3c). LuaCore answers true.
    virtual auto __stdcall IsCore() -> bool = 0;
    // high: from IDirect3DDevice8::BeginScene (0x1005e5b0 -> 0x1005efe6); LuaCore raises "prerender".
    virtual auto __stdcall Prerender() -> void = 0;
    // high: from EndScene (0x1005e630 -> 0x1005f247); LuaCore raises "postrender".
    virtual auto __stdcall Postrender() -> void = 0;
    // high: a console line whose first word matches one of GetCommands (0x10042280): the rest of the line only, no
    // keyword. LuaCore's Callback_HandleCommand(const char*) skips one leading space and prints the `lua` help.
    virtual auto __stdcall HandleCommand(const char* args) -> void = 0;
    // high: a line no plugin keyword took, offered to each plugin in turn (0x1004255f); true stops the search.
    // LuaCore raises "unhandled command" from it.
    virtual auto __stdcall HandleUnhandledCommand(const char* line) -> bool = 0;
    // high: 0x10036336; LuaCore's Callback_IncomingText(const char*, char*, int&), "incoming text".
    virtual auto __stdcall IncomingText(const char* original, char* modified, int* mode) -> void = 0;
    // high: LuaCore's Callback_OutgoingText(const char*, char*, id::command_source), "outgoing text".
    virtual auto __stdcall OutgoingText(const char* original, char* modified, int source) -> void = 0;
    // high: one incoming game packet (0x10070f20); LuaCore's Callback_IncomingChunk(const void*, void*, bool, bool). original is Hook.dll's copy taken before any plugin ran; modified
    // is the packet in the chunk, which plugins may change; blocked is true if an earlier plugin blocked it. Return
    // true to block. The size is the header's: (modified[1] & 0xfe) * 2. Plugins run in load order.
    virtual auto __stdcall IncomingChunk(const uint8_t* original, uint8_t* modified, bool injected, bool blocked) -> bool = 0;
    // high: the same for an outgoing packet.
    virtual auto __stdcall OutgoingChunk(const uint8_t* original, uint8_t* modified, bool injected, bool blocked) -> bool = 0;
    // high: 0x10062238; LuaCore's Callback_HandleMouse is bool(int, int, int, int, bool), "mouse".
    virtual auto __stdcall Mouse(int type, int x, int y, int delta, bool blocked) -> bool = 0;
    // high: 0x100619fe; LuaCore's Callback_HandleKeyboard is bool(int, char, bool), "keyboard".
    virtual auto __stdcall Keyboard(int dik, char pressed, bool blocked) -> bool = 0;
    // medium: item events (0x10071192, 0x10071242), four arguments each; LuaCore raises "add item" and "remove item".
    virtual auto __stdcall AddItem(uint8_t bag, int index, int id, int count) -> void    = 0;
    virtual auto __stdcall RemoveItem(uint8_t bag, int index, int id, int count) -> void = 0;
};

} // namespace memoria::windower

// The two exports Hook.dll resolves by name (0x1006f309, 0x1006f5eb). Both take no arguments; their calling
// convention does not matter.
extern "C" auto GetInterfaceVersion() -> uint32_t;
extern "C" auto CreateInstance() -> memoria::windower::IPlugin*;
