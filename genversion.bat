for /f "tokens=* usebackq" %%x in (`git describe --tags`) do (set version=%%x)
if "%RELEASE_BUILD%"=="" (
    echo #define SAR_VERSION "%version%-canary">Version.hpp
    echo #define SAR_DEV_BUILD 1 >>Version.hpp
    rem this fork's builds are for rendering: no "Activate SAR" watermark and no "SAR is not up-to-date" warning
    rem (sar_update would replace the fork with official SAR) unless SAR_FORK_NAGS is set
    if "%SAR_FORK_NAGS%"=="" echo #define NO_DEV_WATERMARK 1 >>Version.hpp
    if "%SAR_FORK_NAGS%"=="" echo #define NO_UPDATE_WARNING 1 >>Version.hpp
) else (
    echo #define SAR_VERSION "%version%">Version.hpp
)
echo #define SAR_DEMO_SIGN_PUBKEY { %DEMO_SIGN_PUBKEY% }>>Version.hpp
echo #define SAR_DEMO_SIGN_PRIVKEY { %DEMO_SIGN_PRIVKEY% }>>Version.hpp
