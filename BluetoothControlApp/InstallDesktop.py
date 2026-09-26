"""Install the packaged Ubuntu app, icon, and launcher for the current user."""

import argparse
import os
import shutil
import subprocess
import tempfile
from pathlib import Path


# The shared Flet 1.0.1 Linux viewer advertises this identity to GNOME, even
# when the packed launcher passes FLET_APP_ID. The desktop file must match it.
VIEWER_APP_ID = "com.appveyor.flet"


def remove_old_launcher(path: Path, executable: Path) -> None:
    """Remove only the launcher created by the previous version of this script."""
    if not path.is_file():
        return
    contents = path.read_text(encoding="utf-8")
    if "Name=Hydroponics Control\n" in contents and f'Exec="{executable}"\n' in contents:
        path.unlink()


def install_file(source: Path, destination: Path, executable: bool = False) -> None:
    """Replace files atomically, including when the old executable is running."""
    handle, temporary_name = tempfile.mkstemp(prefix=".hydro-install-", dir=destination.parent)
    os.close(handle)
    temporary = Path(temporary_name)
    try:
        shutil.copy2(source, temporary)
        if executable:
            temporary.chmod(temporary.stat().st_mode | 0o111)
        temporary.replace(destination)
    finally:
        temporary.unlink(missing_ok=True)


def main() -> None:
    parser = argparse.ArgumentParser(description="Install Hydroponics Control for this user")
    parser.parse_args()

    source = Path(__file__).resolve().parent
    executable = source / "dist" / "HydroponicsControl"
    icon = source / "AppIcon.png"
    if not executable.is_file() or not icon.is_file():
        parser.error('Build first with: .venv/bin/flet pack Main.py --name HydroponicsControl --icon AppIcon.png --bundle-id com.appveyor.flet --product-name "Hydroponics Control"')

    data_home = Path(os.environ.get("XDG_DATA_HOME", Path.home() / ".local" / "share"))
    install_dir = data_home / "HydroponicsControl"
    applications_dir = data_home / "applications"
    install_dir.mkdir(parents=True, exist_ok=True)
    applications_dir.mkdir(parents=True, exist_ok=True)

    installed_executable = install_dir / "HydroponicsControl"
    installed_icon = install_dir / "AppIcon.png"
    install_file(executable, installed_executable, executable=True)
    install_file(icon, installed_icon)

    launcher = applications_dir / f"{VIEWER_APP_ID}.desktop"
    launcher.write_text(
        "[Desktop Entry]\n"
        "Type=Application\n"
        "Name=Hydroponics Control\n"
        "Comment=Monitor and configure your hydroponics controller\n"
        f'Exec="{installed_executable}"\n'
        f"Icon={installed_icon}\n"
        "Terminal=false\n"
        "Categories=Utility;\n"
        f"StartupWMClass={VIEWER_APP_ID}\n",
        encoding="utf-8",
    )
    launcher.chmod(0o755)
    remove_old_launcher(applications_dir / "HydroponicsControl.desktop", installed_executable)
    if shutil.which("update-desktop-database"):
        subprocess.run(["update-desktop-database", str(applications_dir)], check=False)
    print(f"Application launcher installed: {launcher}")

    desktop_dir = Path.home() / "Desktop"
    remove_old_launcher(desktop_dir / launcher.name, installed_executable)
    remove_old_launcher(desktop_dir / "HydroponicsControl.desktop", installed_executable)


if __name__ == "__main__":
    main()
