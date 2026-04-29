# ntium

## Development setup for Windows 11 25H2

This repository currently contains only this README. There is no application
code, dependency manifest, build script, or test suite to install or run yet.

Use the following baseline setup for Windows 11 25H2 so the machine is ready
when project files are added.

### Recommended environment

- Windows 11 25H2 with current Windows Update patches.
- Windows Terminal.
- WSL2 with Ubuntu.
- Git for Windows.
- Visual Studio Code or Cursor with the WSL extension.

### Install WSL2 and Ubuntu

Open PowerShell as Administrator:

```powershell
wsl --install -d Ubuntu
wsl --set-default-version 2
wsl --update
```

Restart Windows if prompted, then open Ubuntu and create the Linux user account.

### Prepare Ubuntu packages

Run these commands inside Ubuntu:

```bash
sudo apt update
sudo apt upgrade -y
sudo apt install -y build-essential ca-certificates curl git unzip
```

### Clone the repository

Clone into the Linux filesystem for best file-watching and build performance:

```bash
mkdir -p ~/src
cd ~/src
git clone <repository-url> ntium
cd ntium
```

### Open in Cursor or VS Code

From the Ubuntu shell:

```bash
cursor .
```

or:

```bash
code .
```

### Current project status

There are no dependencies to install and no application command to run yet. Once
the project adds a package manifest such as `package.json`, `pyproject.toml`,
`requirements.txt`, `go.mod`, or `Cargo.toml`, install dependencies using that
toolchain's standard command and document the exact command in this README.
