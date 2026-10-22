param([string]$OutputDirectory = "out/Knock")
$ErrorActionPreference = "Stop"
$ProjectRoot = Split-Path -Parent $PSScriptRoot
Push-Location $ProjectRoot
try {
    cmake -S . -B build -A x64
    if ($LASTEXITCODE -ne 0) { throw "CMake configuration failed." }
    cmake --build build --config Release --parallel
    if ($LASTEXITCODE -ne 0) { throw "Compilation failed." }
    ctest --test-dir build -C Release --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw "Tests failed; the release was not packaged." }
    cmake --install build --config Release --prefix $OutputDirectory
    if ($LASTEXITCODE -ne 0) { throw "Packaging failed." }
    Write-Host "Built Knock in $OutputDirectory"
} finally {
    Pop-Location
}
