#!/usr/bin/env python3
"""Create and register a kernel-linked PrismOS C++ built-in app."""

from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parent.parent
BUILTINS = ROOT / "src/apps/builtins"
RUNTIME = ROOT / "src/apps/app_runtime.c"
MANAGER = ROOT / "src/apps/app_manager.c"


def fail(message: str) -> None:
    print(f"new-builtin: error: {message}", file=sys.stderr)
    raise SystemExit(2)


def replace_once(path: Path, old: str, new: str) -> None:
    text = path.read_text()
    if text.count(old) != 1:
        fail(f"could not find a unique registration point in {path}")
    path.write_text(text.replace(old, new))


def main() -> int:
    if len(sys.argv) != 3:
        fail("usage: make new-builtin NAME=weather ID=WEATHER")

    values = {}
    for argument in sys.argv[1:]:
        if "=" not in argument:
            fail("arguments must use NAME=value syntax")
        key, value = argument.split("=", 1)
        values[key] = value

    name = values.get("NAME", "")
    app_id = values.get("ID", "").upper()
    if not re.fullmatch(r"[a-z][a-z0-9_]*", name):
        fail("NAME must be a lowercase C++ identifier such as weather")
    if not re.fullmatch(r"[A-Z][A-Z0-9_]{0,14}", app_id):
        fail("ID must be 1-15 uppercase letters, digits, or underscores")

    function = f"{name}_app_run"
    guard = f"PRISMOS_APPS_BUILTINS_{name.upper()}_APP_HPP"
    header = BUILTINS / f"{name}_app.hpp"
    source = BUILTINS / f"{name}_app.cpp"
    if header.exists() or source.exists():
        fail(f"{name} already exists")

    header.write_text(f"""#ifndef {guard}
#define {guard}

#ifdef __cplusplus
extern "C" {{
#endif

int {function}(const char* args);

#ifdef __cplusplus
}}
#endif

#endif
""")
    source.write_text(f"""#include "apps/builtins/{name}_app.hpp"
#include "apps/builtins/prismcc.hpp"

using namespace PrismCC;

int {function}(const char* args) {{
    (void)args;
    console_writeln("{app_id} app");
    return 0;
}}
""")

    replace_once(
        RUNTIME,
        '#include "apps/builtins/ide_app.hpp"\n',
        f'#include "apps/builtins/ide_app.hpp"\n#include "apps/builtins/{name}_app.hpp"\n',
    )
    replace_once(
        RUNTIME,
        '    ERROR_LOG("app runtime unknown app id");\n',
        f'''    if (string_equals(app_id, "{app_id}")) {{
        DEBUG_LOG("app runtime dispatching {app_id} app");
        return {function}(args);
    }}

    ERROR_LOG("app runtime unknown app id");
''',
    )

    path_macro = f"#define APPS_{name.upper()}_PATH \"/APPS/{app_id}.APP\"\n"
    replace_once(
        MANAGER,
        '#define APPS_BANK_PATH "/APPS/BANK.APP"\n',
        '#define APPS_BANK_PATH "/APPS/BANK.APP"\n' + path_macro,
    )
    package_bytes = ", ".join(f"'{char}'" for char in app_id)
    installer = f"""static int ensure_{name}_app_installed(void) {{
    app_loader_image_t image;
    uint8_t package[PRISM_APP_HEADER_SIZE + {len(app_id)}U];

    if (app_loader_load_image(APPS_{name.upper()}_PATH, &image) == 0) {{
        return 0;
    }}

    write_u32le(&package[0], PRISM_APP_MAGIC);
    write_u16le(&package[4], PRISM_APP_FORMAT_VERSION);
    write_u16le(&package[6], 0U);
    write_u32le(&package[8], 0U);
    write_u32le(&package[12], {len(app_id)}U);
    write_u32le(&package[16], 0U);
"""
    for index, char in enumerate(app_id):
        installer += f"    package[{20 + index}] = '{char}';\n"
    installer += f""" 
    if (vfs_write_file(APPS_{name.upper()}_PATH, (const char*)package, sizeof(package), 0) != 0) {{
        ERROR_LOG("failed to install {name} app package");
        return -1;
    }}

    DEBUG_LOG("{name} app package installed");
    return 0;
}}

"""
    replace_once(MANAGER, "int app_manager_init(void) {\n", installer + "int app_manager_init(void) {\n")
    replace_once(
        MANAGER,
        """    if (ensure_bank_app_installed() != 0) {
        app_manager_ready = 0;
        return -1;
    }
""",
        """    if (ensure_bank_app_installed() != 0) {
        app_manager_ready = 0;
        return -1;
    }

    if (ensure_%s_app_installed() != 0) {
        app_manager_ready = 0;
        return -1;
    }
""" % name,
    )
    replace_once(
        MANAGER,
        """            } else if (strcmp(app_abs_path, APPS_BANK_PATH) == 0) {
                repaired = ensure_bank_app_installed();
            }
""",
        """            } else if (strcmp(app_abs_path, APPS_BANK_PATH) == 0) {
                repaired = ensure_bank_app_installed();
            } else if (strcmp(app_abs_path, APPS_%s_PATH) == 0) {
                repaired = ensure_%s_app_installed();
            }
""" % (name.upper(), name),
    )

    print(f"Created {source.relative_to(ROOT)}")
    print(f"Created {header.relative_to(ROOT)}")
    print(f"Registered /APPS/{app_id}.APP")
    print("Implement the generated run function, then run: make kernel.elf")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
