# Spec Delta

## Purpose

How `Memoria.dll` runs as a Windower 4 plugin beside its Ashita plugin, with one session behind both.

## ADDED Requirements

### Requirement: One session behind both hosts
Everything Memoria does SHALL live in a `Session` that includes no Ashita or Windower header. Each host SHALL only turn its framework's callbacks into `Session` calls and give the session paths, chat output, a log, and a frame tick.

#### Scenario: Ashita after the split
- **WHEN** an Ashita player loads a build with the split and plays a session
- **THEN** chat, `/memoria` commands, and the capture read as before the split

### Requirement: The Windower host checks the host before it starts
Under Windower, Memoria SHALL start its session only when the host reports interface version `0x40004` and the host's methods Memoria calls lie inside Hook.dll. Otherwise it SHALL stay loaded and idle, and say why on Windower's console when it can.

#### Scenario: A host Memoria was not built for
- **WHEN** Hook.dll hands Memoria a host whose version is not `0x40004`
- **THEN** Memoria records nothing, patches nothing, and Windower and the game keep running

### Requirement: Commands under Windower
Under Windower, Memoria SHALL register the command `memoria`, so `memoria <command>` on the console and `//memoria <command>` in the chat line run the same commands as `/memoria <command>` under Ashita, and chat text that names a command SHALL name it as a Windower player types it.

#### Scenario: Status from the chat line
- **WHEN** a Windower player types `//memoria status`
- **THEN** Memoria prints the same status lines as under Ashita, with `//memoria` in any command it names
