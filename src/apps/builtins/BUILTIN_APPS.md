# Built-in Apps

This markdown file will show you how to write&develop build-in apps in PrismOS that come pre-installed with the base PrismOS system.

## Create one

Run this from the repository root:

```sh
make new-builtin NAME=weather ID=WEATHER
```

This creates:

```text
src/apps/builtins/weather_app.cpp
src/apps/builtins/weather_app.hpp
```

It also registers the app in the build, runtime dispatcher, and `/APPS`
package installer.

Build it with:

```sh
make kernel.elf
```

The generated app is installed as `/APPS/WEATHER.APP`.

## Write the app

Open `src/apps/builtins/weather_app.cpp` and replace the generated function:

```cpp
int weather_app_run(const char* args) {
    (void)args;
    console_writeln("WEATHER app");
    return 0;
}
```

Use the shared facade:

```cpp
#include "apps/builtins/prismcc.hpp"

using namespace PrismCC;
```

It provides console, keyboard, clipboard, VFS, logging, string, memory, and
PrismCC compiler functions. Apps are freestanding kernel code, so do not use
the C++ standard library, exceptions, RTTI, threads, `new`, or `delete`.

The entry point must:

- be named `<name>_app_run`;
- accept `const char* args`;
- return `0` on success or a negative value on failure;
- keep the existing C-compatible declaration in the generated `.hpp`.

## Naming

- `NAME` is lowercase and becomes the C++ function name.
- `ID` is uppercase and becomes the `.APP` identifier and filename.
- Keep `ID` at 15 characters or fewer.
- Choose an unused `NAME` and `ID`.

Example:

```sh
make new-builtin NAME=clock ID=CLOCK
make kernel.elf
```

Then edit `src/apps/builtins/clock_app.cpp` and run `/APPS/CLOCK.APP` in
PrismOS.
