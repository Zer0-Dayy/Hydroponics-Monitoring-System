"""Install the packaged Ubuntu app, icon, and launcher for the current user."""

import argparse
import os
import shutil
from pathlib import Path


def main() -> None:
    parser = argparse.ArgumentParser(description="Install Hydroponics Control for this user")
    parser.add_argument("--desktop", action="store_true", help="also add a Desktop shortcut")
    args = parser.parse_args()

    source = Path(__file__).resolve().parent
    executable = source / "dist" / "HydroponicsControl"
    icon = source / "AppIcon.png"
    if not executable.is_file() or not icon.is_file():
        parser.error("Build first with: .venv/bin/flet pack Main.py --name HydroponicsControl --icon AppIcon.png")

    data_home = Path(os.environ.get("XDG_DATA_HOME", Path.home() / ".local" / "share"))
    install_dir = data_home / "HydroponicsControl"
    applications_dir = data_home / "applications"
    install_dir.mkdir(parents=True, exist_ok=True)
    applications_dir.mkdir(parents=True, exist_ok=True)

    installed_executable = install_dir / "HydroponicsControl"
    installed_icon = install_dir / "AppIcon.png"
    shutil.copy2(executable, installed_executable)
    shutil.copy2(icon, installed_icon)
    installed_executable.chmod(installed_executable.stat().st_mode | 0o111)

    launcher = applications_dir / "HydroponicsControl.desktop"
    launcher.write_text(
        "[Desktop Entry]\n"
        "Type=Application\n"
        "Name=Hydroponics Control\n"
        "Comment=Monitor and configure your hydroponics controller\n"
        f'Exec="{installed_executable}"\n'
        f"Icon={installed_icon}\n"
        "Terminal=false\n"
        "Categories=Utility;\n"
        "StartupWMClass=HydroponicsControl\n",
        encoding="utf-8",
    )
    launcher.chmod(0o755)
    print(f"Application launcher installed: {launcher}")

    if args.desktop:
        desktop_dir = Path.home() / "Desktop"
        if desktop_dir.is_dir():
            desktop_launcher = desktop_dir / launcher.name
            shutil.copy2(launcher, desktop_launcher)
            desktop_launcher.chmod(0o755)
            print(f"Desktop shortcut added: {desktop_launcher}")
            print("On Ubuntu, right-click the shortcut and choose Allow Launching if prompted.")
        else:
            print("No ~/Desktop folder found; the app is available from the application menu.")


if __name__ == "__main__":
    main()
