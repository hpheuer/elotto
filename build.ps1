# idf.py wrapper — sets the ESP-IDF environment and forwards all arguments.
#
#   .\build.ps1 build
#   .\build.ps1 -C ota_firmware build      # -C selects another project
#
# Firmware is delivered over OTA; do not flash over serial from this script.
#
# WHY THIS EXISTS: env vars do not survive between shell invocations, so every
# call would otherwise have to re-export IDF_PATH, the venv and the tool paths.
#
# WHY THE VENV MATTERS: this uses the **VS Code extension's** interpreter
# (C:\Espressif\tools\python\v6.0.1\venv), NOT export.ps1's
# C:\Espressif\python_env\idf6.0_py3.11_env. A build directory is pinned to
# whichever ran first; mixing them fails with
#   "'...idf6.0_py3.11_env\python.exe' is currently active while the project was
#    configured with '...tools\python\v6.0.1\venv\python.exe'. Run 'idf.py fullclean'"
# This script previously used the export.ps1 env, which is that bug.
$IdfPath  = "C:\esp\v6.0.1\esp-idf"
$VenvPy   = "C:\Espressif\tools\python\v6.0.1\venv\Scripts\python.exe"

$env:IDF_PATH                     = $IdfPath
$env:IDF_TOOLS_PATH               = "C:\Espressif\tools"
$env:IDF_PYTHON_ENV_PATH          = "C:\Espressif\tools\python\v6.0.1\venv"
$env:ESP_IDF_VERSION              = "6.0"
$env:ESP_ROM_ELF_DIR              = "C:\Espressif\tools\esp-rom-elfs\20241011\"
# The extension's venv ships rich 15 while IDF pins <14.3.4; the check is
# cosmetic here and would otherwise abort every build.
$env:IDF_PYTHON_CHECK_CONSTRAINTS = "no"
$env:PATH = "C:\Espressif\tools\cmake\4.0.3\bin;" +
            "C:\Espressif\tools\ninja\1.12.1;" +
            "C:\Espressif\tools\riscv32-esp-elf\esp-15.2.0_20251204\riscv32-esp-elf\bin;" +
            "C:\Espressif\tools\ccache\4.12.1\ccache-4.12.1-windows-x86_64;" +
            "C:\Espressif\tools\git\bin;" +
            $env:PATH

Set-Location $PSScriptRoot
& $VenvPy "$IdfPath\tools\idf.py" @args
