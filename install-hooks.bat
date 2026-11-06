@echo off
setlocal
REM ---------------------------------------------------------------------------
REM Install the repository's git hooks. Run once after cloning (and again after
REM pulling changes to .githooks/).
REM
REM Uses core.hooksPath instead of copying files into .git\hooks, so the hooks
REM stay versioned in the repo and updates apply immediately.
REM ---------------------------------------------------------------------------
cd /d "%~dp0"

where git >nul 2>nul
if errorlevel 1 (
    echo [ERROR] git not found on PATH.
    exit /b 1
)

git rev-parse --is-inside-work-tree >nul 2>nul
if errorlevel 1 (
    echo [ERROR] Not inside a git repository: %CD%
    exit /b 1
)

git config core.hooksPath .githooks
if errorlevel 1 (
    echo [ERROR] Failed to set core.hooksPath.
    exit /b 1
)

REM Git for Windows runs hooks through its own sh; make sure they are executable.
git update-index --chmod=+x .githooks/pre-commit >nul 2>nul

echo [OK] Hooks installed (core.hooksPath = .githooks)
echo      pre-commit bumps the patch version in src\CMakeLists.txt on each commit.
echo      Use "git commit --no-verify" to commit without a bump.
echo      If you edit the version in src\CMakeLists.txt yourself, that value is kept.
exit /b 0
