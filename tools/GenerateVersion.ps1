<#
.SYNOPSIS
    Generates res/Version.h with the version and git information for MinFlyout.

.DESCRIPTION
    Runs before every build: the pre-build event of minflyout.vcxproj and the
    "version" target of CMakeLists.txt. Taken over from ClipWatch, so both
    projects describe their builds the same way. The base version comes from
    Version.txt, everything else is queried from git:

      * the abbreviated commit hash (7 characters, as GitKraken shows it),
      * the full commit hash,
      * the current branch, or "detached" for a detached HEAD,
      * the number of commits reachable from HEAD, used as the fourth field
        of FILEVERSION / PRODUCTVERSION and of the version strings,
      * whether the working tree has uncommitted changes.

    The version itself stays purely numeric (major.minor.patch.count); the
    commit and the dirty state are carried by VER_BRANCH, VER_COMMIT_SHORT,
    VER_COMMIT_FULL and VER_DIRTY_SUFFIX, which the resource script composes
    into VER_FILEDESCRIPTION_STR. That one is composed here rather than in the
    resource script, because windres (MinGW) does not join adjacent string
    literals the way rc.exe does.

    Version.h is only rewritten when its content actually changes, so an
    unchanged repository does not force a rebuild of the resources on every
    build.

    Missing git information never breaks the build: the script falls back to
    0.0.0.unknown and reports a warning in MSBuild's format.

.PARAMETER ProjectDir
    Directory of the project, used as the git working directory.

.PARAMETER OutputPath
    Path of the generated header.

.PARAMETER DoxygenPath
    Path of the generated doxygen fragment, which carries PROJECT_NUMBER and
    is pulled in by MinFlyout.dox through @INCLUDE. Empty writes none.

.PARAMETER VersionFile
    Text file holding the base version as a single "major.minor.patch" line.

.PARAMETER UntrackedFiles
    How untracked files affect the dirty state: "normal" counts them (files
    excluded by .gitignore never count), "no" looks at tracked files only.

.EXAMPLE
    powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools\GenerateVersion.ps1

.NOTES
    Written for Windows PowerShell 5.1 as well as PowerShell 7, so it avoids
    7-only syntax.

    The script lives in the tools subdirectory, so the defaults below resolve
    against its parent, the project directory.
#>

[CmdletBinding()]
param(
    [string] $ProjectDir   = (Split-Path -Parent $PSScriptRoot),
    [string] $OutputPath   = (Join-Path (Split-Path -Parent $PSScriptRoot) 'res\Version.h'),
    [string] $DoxygenPath  = (Join-Path (Split-Path -Parent $PSScriptRoot) 'Version.doxyfile'),
    [string] $VersionFile  = (Join-Path (Split-Path -Parent $PSScriptRoot) 'Version.txt'),

    [ValidateSet('normal', 'no')]
    [string] $UntrackedFiles = 'normal'
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

# Largest value a FILEVERSION field can hold.
$script:MaxVersionField = 65535

function Write-BuildWarning
{
    param([string] $Message)

    # MSBuild's message format, so the warning shows up in the Error List.
    Write-Host ('GenerateVersion.ps1 : warning VER001 : {0}' -f $Message)
}

function Write-BuildError
{
    param([string] $Message)

    Write-Host ('GenerateVersion.ps1 : error VER002 : {0}' -f $Message)
}

<#
.SYNOPSIS
    Reads major.minor.patch from the version file.
#>
function Get-BaseVersion
{
    param([string] $Path)

    $version = @{ Major = 0; Minor = 0; Patch = 0 }

    if (-not (Test-Path -LiteralPath $Path))
    {
        Write-BuildWarning ("'{0}' not found, falling back to 0.0.0." -f $Path)
        return $version
    }

    $content = Get-Content -LiteralPath $Path -ErrorAction SilentlyContinue
    $line = ''
    foreach ($candidate in $content)
    {
        if (-not [string]::IsNullOrWhiteSpace($candidate))
        {
            $line = $candidate
            break
        }
    }

    if ($line -notmatch '^\s*(\d{1,5})\.(\d{1,5})\.(\d{1,5})\s*$')
    {
        Write-BuildWarning ("'{0}' does not contain a 'major.minor.patch' line, falling back to 0.0.0." -f $Path)
        return $version
    }

    $version.Major = [int] $Matches[1]
    $version.Minor = [int] $Matches[2]
    $version.Patch = [int] $Matches[3]

    foreach ($key in @('Major', 'Minor', 'Patch'))
    {
        if ($version[$key] -gt $script:MaxVersionField)
        {
            Write-BuildWarning ("{0} version {1} exceeds {2} and was clamped." -f $key, $version[$key], $script:MaxVersionField)
            $version[$key] = $script:MaxVersionField
        }
    }

    return $version
}

<#
.SYNOPSIS
    Runs git and returns its trimmed output, or $null when it failed.
#>
function Invoke-Git
{
    param([string[]] $Arguments)

    try
    {
        $output = & git @Arguments 2>$null
        if ($LASTEXITCODE -ne 0)
        {
            return $null
        }
        return (($output | Out-String) -replace '\s+$', '')
    }
    catch
    {
        return $null
    }
}

<#
.SYNOPSIS
    Collects commit hash, commit count and dirty state.
#>
function Get-GitInformation
{
    param(
        [string] $WorkingDirectory,
        [string] $Untracked
    )

    $info = @{
        Full      = 'unknown'
        Short     = 'unknown'
        Branch    = 'unknown'
        Count     = 0
        Dirty     = $false
        Available = $false
    }

    if ($null -eq (Get-Command 'git' -ErrorAction SilentlyContinue))
    {
        Write-BuildWarning 'git was not found in PATH, the version has no commit information.'
        return $info
    }

    $common = @('-C', $WorkingDirectory)

    $full = Invoke-Git ($common + @('rev-parse', 'HEAD'))
    if ([string]::IsNullOrWhiteSpace($full))
    {
        Write-BuildWarning ("'{0}' is not a git repository or has no commit yet." -f $WorkingDirectory)
        return $info
    }

    $info.Full      = $full
    $info.Available = $true

    $short = Invoke-Git ($common + @('rev-parse', '--short=7', 'HEAD'))
    if (-not [string]::IsNullOrWhiteSpace($short))
    {
        $info.Short = $short
    }
    else
    {
        # Fall back to the first seven characters of the full hash.
        $info.Short = $full.Substring(0, [Math]::Min(7, $full.Length))
    }

    # symbolic-ref fails on a detached HEAD, which is exactly how it is told
    # apart from being on a branch.
    $branch = Invoke-Git ($common + @('symbolic-ref', '--short', '-q', 'HEAD'))
    if ([string]::IsNullOrWhiteSpace($branch))
    {
        $info.Branch = 'detached'
    }
    else
    {
        $info.Branch = $branch
    }

    $count = Invoke-Git ($common + @('rev-list', '--count', 'HEAD'))
    if ($count -match '^\d+$')
    {
        $value = [long] $count
        if ($value -gt $script:MaxVersionField)
        {
            Write-BuildWarning ("The commit count {0} exceeds {1} and was clamped in FILEVERSION." -f $value, $script:MaxVersionField)
            $value = $script:MaxVersionField
        }
        $info.Count = [int] $value
    }
    else
    {
        Write-BuildWarning 'The commit count could not be determined, using 0.'
    }

    # "git status --porcelain" prints one line per change and nothing at all
    # for a clean working tree, so an empty result means clean.
    $status = Invoke-Git ($common + @('status', '--porcelain', ('--untracked-files=' + $Untracked)))
    if ($null -eq $status)
    {
        Write-BuildWarning 'The working tree state could not be determined, assuming clean.'
    }
    else
    {
        $info.Dirty = -not [string]::IsNullOrWhiteSpace($status)
    }

    return $info
}

<#
.SYNOPSIS
    Builds the content of Version.h.
#>
function New-VersionHeader
{
    param(
        [hashtable] $Version,
        [hashtable] $Git
    )

    $versionString = '{0}.{1}.{2}.{3}' -f $Version.Major, $Version.Minor, $Version.Patch, $Git.Count

    $dirtyFlag = 0
    $dirtyFlags = '0x0L'
    $dirtySuffix = ''
    if ($Git.Dirty)
    {
        $dirtyFlag = 1
        $dirtyFlags = '0x8L'   # VS_FF_PRIVATEBUILD
        $dirtySuffix = ' (dirty)'
    }

    $lines = @(
        '/**',
        ' * \file Version.h',
        ' * \ingroup app',
        ' * \brief Version and git information for MinFlyout.',
        ' *',
        ' * GENERATED FILE - DO NOT EDIT AND DO NOT COMMIT.',
        ' *',
        ' * Written by tools/GenerateVersion.ps1 before every build. Change the',
        ' * base version in Version.txt; everything else comes from git.',
        ' */',
        '',
        '#pragma once',
        '',
        '// Base version, from Version.txt.',
        ('#define VER_MAJOR {0}' -f $Version.Major),
        ('#define VER_MINOR {0}' -f $Version.Minor),
        ('#define VER_PATCH {0}' -f $Version.Patch),
        '',
        '// Git information.',
        '',
        '/// Commits reachable from HEAD; the fourth version field.',
        ('#define VER_COMMIT_COUNT {0}' -f $Git.Count),
        '',
        '/// Abbreviated commit hash, seven characters as GitKraken shows it.',
        ('#define VER_COMMIT_SHORT "{0}"' -f $Git.Short),
        '',
        '/// Full commit hash.',
        ('#define VER_COMMIT_FULL "{0}"' -f $Git.Full),
        '',
        '/// Current branch, or "detached" for a detached HEAD.',
        ('#define VER_BRANCH "{0}"' -f $Git.Branch),
        '',
        '/// 1 when the working tree had uncommitted changes at build time.',
        ('#define VER_DIRTY {0}' -f $dirtyFlag),
        '',
        '/// " (dirty)" for an unclean build, an empty string otherwise.',
        ('#define VER_DIRTY_SUFFIX "{0}"' -f $dirtySuffix),
        '',
        '// Composed values for the resource script and the about dialog.',
        '',
        '/// Numeric version; a hash does not fit into a 16 bit field.',
        '#define VER_FILEVERSION    VER_MAJOR,VER_MINOR,VER_PATCH,VER_COMMIT_COUNT',
        '#define VER_PRODUCTVERSION VER_FILEVERSION',
        '',
        '/// Displayed version, matching the numeric one.',
        ('#define VER_VERSION_STR "{0}"' -f $versionString),
        '#define VER_FILEVERSION_STR    VER_VERSION_STR',
        '#define VER_PRODUCTVERSION_STR VER_VERSION_STR',
        '',
        '/// FileDescription of the version resource.',
        ('#define VER_FILEDESCRIPTION_STR "MinFlyout - {0}: {1}{2}"' -f $Git.Branch, $Git.Short, $dirtySuffix),
        '',
        '/// VS_FF_PRIVATEBUILD for a dirty build, 0 otherwise.',
        ('#define VER_FILEFLAGS_DIRTY {0}' -f $dirtyFlags),
        '',
        '/// Turns a narrow string literal macro into a wide one.',
        '#define VER_WIDEN_(text) L ## text',
        '#define VER_WIDEN(text)  VER_WIDEN_(text)',
        ''
    )

    return (($lines -join "`r`n"))
}

<#
.SYNOPSIS
    Builds the content of the doxygen fragment.

.DESCRIPTION
    Only PROJECT_NUMBER, so the documentation header states which build it
    was generated from. MinFlyout.dox pulls the file in with @INCLUDE after
    its own PROJECT_NUMBER, and the later assignment wins.
#>
function New-DoxygenFragment
{
    param(
        [hashtable] $Version,
        [hashtable] $Git
    )

    $versionString = '{0}.{1}.{2}.{3}' -f $Version.Major, $Version.Minor, $Version.Patch, $Git.Count

    $dirtySuffix = ''
    if ($Git.Dirty)
    {
        $dirtySuffix = ' (dirty)'
    }

    $lines = @(
        '# Version information for the MinFlyout Doxyfile.',
        '#',
        '# GENERATED FILE - DO NOT EDIT AND DO NOT COMMIT.',
        '#',
        '# Written by tools/GenerateVersion.ps1 before every build and pulled in',
        '# by MinFlyout.dox through "@INCLUDE = Version.doxyfile".',
        '',
        # Same shape as VER_FILEDESCRIPTION_STR, and without
        # nested parentheses for a dirty build.
        ('PROJECT_NUMBER         = "{0} - {1}: {2}{3}"' -f `
            $versionString, $Git.Branch, $Git.Short, $dirtySuffix),
        ''
    )

    return (($lines -join "`r`n"))
}

<#
.SYNOPSIS
    Writes a file only when its content differs from what is already there.

.OUTPUTS
    "written", "unchanged" or "failed".
#>
function Save-IfChanged
{
    param(
        [string] $Path,
        [string] $Content
    )

    $existing = $null
    if (Test-Path -LiteralPath $Path)
    {
        $existing = Get-Content -LiteralPath $Path -Raw -ErrorAction SilentlyContinue
    }

    if ($existing -eq $Content)
    {
        return 'unchanged'
    }

    try
    {
        # ASCII keeps rc.exe, the compiler and doxygen happy: no byte order mark.
        Set-Content -LiteralPath $Path -Value $Content -Encoding ASCII -NoNewline
    }
    catch
    {
        Write-BuildError ("'{0}' could not be written: {1}" -f $Path, $_.Exception.Message)
        return 'failed'
    }

    return 'written'
}

# --- Main ------------------------------------------------------------------

$baseVersion = Get-BaseVersion -Path $VersionFile
$gitInfo     = Get-GitInformation -WorkingDirectory $ProjectDir -Untracked $UntrackedFiles

$headerState  = Save-IfChanged -Path $OutputPath  -Content (New-VersionHeader -Version $baseVersion -Git $gitInfo)
$doxygenState = 'skipped'
if (-not [string]::IsNullOrWhiteSpace($DoxygenPath))
{
    $doxygenState = Save-IfChanged -Path $DoxygenPath -Content (New-DoxygenFragment -Version $baseVersion -Git $gitInfo)
}

$dirtyState = ''
if ($gitInfo.Dirty)
{
    $dirtyState = ' (dirty)'
}

Write-Host ('Version {0}.{1}.{2}.{3} from {4}: {5}{6} - Version.h {7}, Version.doxyfile {8}.' -f `
    $baseVersion.Major, $baseVersion.Minor, $baseVersion.Patch, $gitInfo.Count, `
    $gitInfo.Branch, $gitInfo.Short, $dirtyState, $headerState, $doxygenState)

if ($headerState -eq 'failed' -or $doxygenState -eq 'failed')
{
    exit 1
}

exit 0
