@REM Copyright (c) Microsoft. All rights reserved.
@REM Licensed under the MIT license. See LICENSE file in the project root for full license information.

setlocal

ver
msbuild -version

set build-root=%~dp0..
rem // resolve to fully qualified path
for %%i in ("%build-root%") do set build-root=%%~fi

REM -- C --
cd %build-root%\build_all\windows
call build.cmd --run-unittests --run-e2e-tests --run-sfc-tests --build-traceabilitytool --provisioning --use-edge-modules --config Release %*
if errorlevel 1 goto :eof
cd %build-root%
