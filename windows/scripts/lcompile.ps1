# Generate and compile dll files
# Default is to compile 64-bit dll, if 32-bit is specified, 32-bit dll would be compiled
$currentDirectory = Get-Location
$cmakeListsPath = Join-Path -Path $currentDirectory -ChildPath "CMakeLists.txt"

if (-not (Test-Path $cmakeListsPath))
{
    Write-Host("No CMakeLists.txt in current directory, please check.")
    return
}

Write-Host "Start generating and compiling..."


$arch = $args[0]

if ($arch -eq "32")
{
    $buildFolderPath = ".\build32"

    if (-not (Test-Path $buildFolderPath))
    {
        New-Item -ItemType Directory -Path $buildFolderPath | Out-Null
        Write-Host "build32 folder created."
    }

    cmake --preset=for32

    if ($LASTEXITCODE -eq 0)
    {
        cmake --build ./build32/
    }
} elseif ($arch -eq "64")
{
    $buildFolderPath = ".\build64"

    if (-not (Test-Path $buildFolderPath))
    {
        New-Item -ItemType Directory -Path $buildFolderPath | Out-Null
        Write-Host "build64 folder created."
    }

    cmake --preset=for64

    if ($LASTEXITCODE -eq 0)
    {
        cmake --build ./build64/
    }
} else
{ # Default is 64-bit
    $buildFolderPath = ".\build64"

    if (-not (Test-Path $buildFolderPath))
    {
        New-Item -ItemType Directory -Path $buildFolderPath | Out-Null
        Write-Host "build64 folder created."
    }

    cmake --preset=for64

    if ($LASTEXITCODE -eq 0)
    {
        cmake --build ./build64/
    }
}
