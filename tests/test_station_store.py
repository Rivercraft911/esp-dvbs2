"""NVS behaviour using the actual firmware persistence code and a fault-injection stub."""
import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
COMP = ROOT / "firmware/components/station_id"
with tempfile.TemporaryDirectory() as directory:
    tmp = pathlib.Path(directory)
    (tmp / "esp_err.h").write_text('''#pragma once
    typedef int esp_err_t;
    #define ESP_OK 0
    #define ESP_ERR_INVALID_ARG 1
    #define ESP_ERR_INVALID_STATE 2
    #define ESP_ERR_NVS_NOT_FOUND 3
    ''')
    (tmp / "nvs_flash.h").write_text('int nvs_flash_init(void);\n')
    (tmp / "nvs.h").write_text('''#pragma once
    #include <stddef.h>
    #include "esp_err.h"
    typedef int nvs_handle_t;
    #define NVS_READWRITE 1
    int nvs_open(const char *, int, nvs_handle_t *);
    int nvs_get_str(nvs_handle_t, const char *, char *, size_t *);
    int nvs_set_str(nvs_handle_t, const char *, const char *);
    int nvs_erase_key(nvs_handle_t, const char *);
    int nvs_commit(nvs_handle_t);
    void nvs_close(nvs_handle_t);
    ''')
    command_source = (ROOT / "firmware/main/station_commands.c").read_text()
    command_source = command_source[command_source.index("bool station_require("):command_source.index("typedef struct {")]
    console = tmp / "console.c"
    console.write_text('#include <stdbool.h>\n#include <stdio.h>\n#include <string.h>\n'
                       '#include "station_store.h"\nconst char *esp_err_to_name(int);\n' + command_source)
    exe = tmp / "store"
    subprocess.run(["cc", "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-Wall", "-Wextra", "-Werror", "-I", str(tmp),
                    "-I", str(COMP / "include"), str(COMP / "station_store.c"),
                    str(COMP / "station_id.c"), str(ROOT / "tests/station_store_host.c"), str(console),
                    "-lm", "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
