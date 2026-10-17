#!/usr/bin/env bash
set -euo pipefail

# Ставит hgs-tray в домашний каталог, без root
# (модель для этого скрипта), которому нужен /usr/local/bin и системный polkit-файл.
# Веток две, по `uname -s`: на Linux это бинарь в ~/.local/bin + systemd --user-юнит, на
# macOS -- .app-бандл в ~/Applications + LaunchAgent в ~/Library/LaunchAgents. Обе
# идемпотентны: каждый шаг сравнивает то, что уже стоит, с тем, что должно быть, и
# печатает "Installed" в конце, только если реально что-то поменял -- это единственная
# строка, которую парсит changed_when роли hgs_sessions
# (roles/hgs_sessions/tasks/main.yml), так что менять её текст здесь и там нужно синхронно.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CHANGED=0
# A changed build replaces the installed GUI through update-installed.py: it quits a
# running GUI like its Quit action (drafts are saved), keeps one rollback copy, starts
# it again and checks drafts and native agents. HGS_TRAY_RESTART=0 only stages the
# build on disk; the running GUI keeps its drafts and the new UI opens on next launch.
RESTART_RUNNING="${HGS_TRAY_RESTART:-1}"
UPDATE=0
case "$RESTART_RUNNING" in 0|1) ;; *) echo "HGS_TRAY_RESTART must be 0 or 1" >&2; exit 1 ;; esac
JOBS="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"

echo "=== hgs-tray installer ==="

if [ "$(uname -s)" = "Darwin" ]; then

# =============================== macOS =======================================
LABEL="com.hgdev.hgs-tray"
APP_DEST="${HOME}/Applications/hgs-tray.app"
BUILD_APP="${SCRIPT_DIR}/build/hgs-tray.app"
PLIST_SRC="${SCRIPT_DIR}/packaging/${LABEL}.plist"
PLIST_DEST="${HOME}/Library/LaunchAgents/${LABEL}.plist"
RELOAD=0

# Неинтерактивный ssh на мак приходит с голым PATH (/usr/bin:/bin:/usr/sbin:/sbin):
# /opt/homebrew/bin туда добавляет только `brew shellenv` из профиля интерактивной
# оболочки. Скрипт должен работать и по ssh, и из Терминала, поэтому инструмент ищем
# сначала в PATH, а потом по штатным путям Homebrew (arm64, затем Intel).
find_tool() {
    if command -v "$1" >/dev/null 2>&1; then command -v "$1"
    elif [ -x "/opt/homebrew/bin/$1" ]; then echo "/opt/homebrew/bin/$1"
    elif [ -x "/usr/local/bin/$1" ]; then echo "/usr/local/bin/$1"
    else return 1
    fi
}

# Сравнить два бандла побайтово нечем: cmp на исполняемом файле упёрся бы в подпись, а
# `diff -r` -- ещё и в _CodeSignature/. cdhash -- это хэш CodeDirectory, то есть кода,
# Info.plist и запечатанных ресурсов разом: одинаковый бандл -> одинаковый cdhash.
# Нет бандла (или он без подписи) -- пустая строка, она не совпадёт ни с чем.
bundle_cdhash() {
    codesign -d --verbose=4 "$1" 2>&1 | sed -n 's/^CDHash=//p'
}

# --- 1. build -----------------------------------------------------------------
echo "[1/4] Building..."
CMAKE="$(find_tool cmake)" || { echo "hgs-tray: cmake not found (brew install cmake)" >&2; exit 1; }
BREW="$(find_tool brew)"   || { echo "hgs-tray: brew not found -- Qt6 comes from it" >&2; exit 1; }
# An Xcode or Command Line Tools update removes the SDK a cached configuration
# names, and Qt's imported targets then fail to configure. Start from a clean
# cache when any cached SDK is gone; sources and build outputs stay.
stale_sdk() {
    local sdk
    [ -f "$1" ] || return 1
    while IFS= read -r sdk; do
        [ -d "${sdk}" ] || return 0
    done < <(grep -o '/[^][;:"=]*\.sdk' "$1" | sort -u)
    return 1
}
if stale_sdk "${SCRIPT_DIR}/build/CMakeCache.txt"; then
    echo "      Cached macOS SDK is gone (Xcode or Command Line Tools updated); reconfiguring."
    rm -rf "${SCRIPT_DIR}/build/CMakeCache.txt" "${SCRIPT_DIR}/build/CMakeFiles"
fi
"${CMAKE}" -B "${SCRIPT_DIR}/build" -S "${SCRIPT_DIR}" \
    -DCMAKE_PREFIX_PATH="$("${BREW}" --prefix qt)" -DCMAKE_BUILD_TYPE=Release
"${CMAKE}" --build "${SCRIPT_DIR}/build" -j"${JOBS}"

# --- 2. .app bundle -------------------------------------------------------------
echo "[2/4] Installing the .app bundle to ~/Applications..."
# Sign before copying. Certificate signing keeps privacy identity stable across
# source rebuilds; incompatible or missing identities stop before replacement.
python3 "${SCRIPT_DIR}/sign-local-macos.py" "${BUILD_APP}" "${APP_DEST}"
mkdir -p "${HOME}/Applications"
if [ "$(bundle_cdhash "${BUILD_APP}")" != "$(bundle_cdhash "${APP_DEST}")" ]; then
    if [ "${RESTART_RUNNING}" = "1" ]; then
        # Replaced in step 4, once the LaunchAgent below is current.
        UPDATE=1
    else
        # rm перед cp: cp -R поверх существующего бандла смешал бы старые файлы с новыми.
        rm -rf "${APP_DEST}"
        cp -R "${BUILD_APP}" "${APP_DEST}"
        # Refresh this bundle's registration after replacing its icon/resources.
        # Do not reset LaunchServices globally or restart the user's Dock.
        LSREGISTER="/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister"
        if [ -x "${LSREGISTER}" ]; then "${LSREGISTER}" -f "${APP_DEST}"; fi
        RELOAD=1
    fi
    CHANGED=1
fi

# --- 3. LaunchAgent -------------------------------------------------------------
echo "[3/4] Installing the LaunchAgent to ~/Library/LaunchAgents..."
# launchd не разворачивает в plist ни ~, ни переменные окружения, поэтому путь к бандлу и
# путь к hgs подставляются здесь -- тем же приёмом, каким linux-ветка ниже правит Exec= в
# .desktop. ~/.local/bin в PATH у launchd нет вовсе, так что --hgs обязателен: без него
# трей не находит hgs и молча показывает пустой флот.
if [ -x "${HOME}/.local/bin/hgs" ]; then
    HGS_BIN="${HOME}/.local/bin/hgs"
elif command -v hgs >/dev/null 2>&1; then
    HGS_BIN="$(command -v hgs)"
else
    echo "hgs-tray: hgs not found -- neither ~/.local/bin/hgs nor in PATH." >&2
    echo "  Install it from the workstation (./mac-install.sh [ssh-alias]) and retry:" >&2
    echo "  without hgs the tray has nothing to show, so nothing is installed." >&2
    exit 1
fi
# stderr агента (StandardErrorPath в plist): туда же, куда macOS складывает логи
# пользовательских программ. Без файла launchd молча выбрасывает stderr, и зависание
# главного потока (см. MainThreadWatchdog.h) не оставляет ни строчки.
LOG_FILE="${HOME}/Library/Logs/hgs-tray.log"
mkdir -p "${HOME}/Library/LaunchAgents" "${HOME}/Library/Logs"
NEW_PLIST="$(sed -e "s|__APP__|${APP_DEST}|g" -e "s|__HGS__|${HGS_BIN}|g" \
                -e "s|__LOG__|${LOG_FILE}|g" "${PLIST_SRC}")"
if [ "$(cat "${PLIST_DEST}" 2>/dev/null || true)" != "${NEW_PLIST}" ]; then
    printf '%s\n' "${NEW_PLIST}" > "${PLIST_DEST}"
    RELOAD=1
    CHANGED=1
fi

# --- 4. load --------------------------------------------------------------------
echo "[4/4] Loading ${LABEL}..."
if [ "${UPDATE}" = "1" ]; then
    # Quits a running GUI, swaps the bundle and bootstraps the current LaunchAgent.
    python3 "${SCRIPT_DIR}/update-installed.py" "${BUILD_APP}" "${APP_DEST}"
    RELOAD=0
fi
# launchctl list печатает "PID<TAB>Status<TAB>Label"; PID "-" значит, что агент
# зарегистрирован, но не работает (Status -- код его последнего выхода).
AGENT_PID="$(launchctl list 2>/dev/null | awk -v l="${LABEL}" '$3 == l { print $1 }')"
if { [ "${RELOAD}" = "1" ] && [ "${RESTART_RUNNING}" = "1" ]; } || [ -z "${AGENT_PID}" ] || [ "${AGENT_PID}" = "-" ]; then
    # У launchctl нет "перезагрузить", а load поверх уже загруженного лейбла -- ошибка,
    # отсюда пара unload+load. На первом прогоне выгружать нечего, поэтому || true.
    launchctl unload "${PLIST_DEST}" 2>/dev/null || true
    launchctl load "${PLIST_DEST}"
    CHANGED=1
fi
# Живой агент, у которого ничего не поменялось, не трогается вовсе -- ровно как
# `systemctl --user enable --now` на активном юните в linux-ветке.

HINT_SELFTEST="Check without the menu bar:  ${APP_DEST}/Contents/MacOS/hgs-tray --selftest --hgs ${HGS_BIN}"
HINT_STATUS="Agent status:                launchctl list | grep ${LABEL}"
HINT_LOG="Agent stderr:                ${LOG_FILE}"

else

# =============================== Linux =======================================
# --- 1. build -----------------------------------------------------------------
echo "[1/4] Building..."
cmake -B "${SCRIPT_DIR}/build" -S "${SCRIPT_DIR}" -DCMAKE_BUILD_TYPE=Release
cmake --build "${SCRIPT_DIR}/build" -j"${JOBS}"

# --- 2. binary ------------------------------------------------------------------
echo "[2/4] Installing binary to ~/.local/bin..."
BIN_DEST="${HOME}/.local/bin/hgs-tray"
RELOAD=0
mkdir -p "${HOME}/.local/bin"
if ! cmp -s "${SCRIPT_DIR}/build/hgs-tray" "${BIN_DEST}" 2>/dev/null; then
    if [ "${RESTART_RUNNING}" = "1" ]; then
        # Replaced in step 4, once the unit below is current.
        UPDATE=1
    else
        install -Dm755 "${SCRIPT_DIR}/build/hgs-tray" "${BIN_DEST}.new"
        mv -f "${BIN_DEST}.new" "${BIN_DEST}"
        RELOAD=1
    fi
    CHANGED=1
fi

# The optional embedded DeepSeek native UI: the build makes it when Qt WebEngine
# is installed (HGS_NATIVE_UI_PLUGIN in CMakeLists.txt). It is placed before the
# GUI is replaced; a running GUI keeps the copy it already loaded. Without it the
# native UI opens in the browser, and an older plugin is removed so that it never
# runs with a newer GUI.
PLUGIN_BUILD="${SCRIPT_DIR}/build/plugins/libhgs-tray-native-ui.so"
PLUGIN_DEST="${HOME}/.local/lib/hgs-tray/plugins/libhgs-tray-native-ui.so"
if [ -f "${PLUGIN_BUILD}" ]; then
    if ! cmp -s "${PLUGIN_BUILD}" "${PLUGIN_DEST}" 2>/dev/null; then
        install -Dm755 "${PLUGIN_BUILD}" "${PLUGIN_DEST}.new"
        mv -f "${PLUGIN_DEST}.new" "${PLUGIN_DEST}"
        CHANGED=1
    fi
elif [ -e "${PLUGIN_DEST}" ]; then
    rm -f "${PLUGIN_DEST}"
    CHANGED=1
fi

# --- 3. systemd --user unit + autostart .desktop -------------------------------
echo "[3/4] Installing the systemd --user unit and the .desktop launcher..."
ICON_ROOT="${HOME}/.local/share/icons/hicolor"
ICON_CHANGED=0
DESKTOP_CHANGED=0
for size in 16 24 32 48 64 128 256 512 1024; do
    ICON_SOURCE="${SCRIPT_DIR}/resources/icons/hgs-zerus-${size}.png"
    ICON_DEST="${ICON_ROOT}/${size}x${size}/apps/hgs-zerus.png"
    if ! cmp -s "${ICON_SOURCE}" "${ICON_DEST}" 2>/dev/null; then
        install -Dm644 "${ICON_SOURCE}" "${ICON_DEST}"
        ICON_CHANGED=1
        CHANGED=1
    fi
done
SYMBOLIC_SOURCE="${SCRIPT_DIR}/resources/icons/hgs-zerus-symbolic.svg"
# A distinct name for the selected mark also invalidates Plasma's cached task
# icon; changing SVG bytes at the previous Z path does not refresh every panel.
SYMBOLIC_DEST="${ICON_ROOT}/scalable/apps/hgs-zerus-swarm-symbolic.svg"
if ! cmp -s "${SYMBOLIC_SOURCE}" "${SYMBOLIC_DEST}" 2>/dev/null; then
    install -Dm644 "${SYMBOLIC_SOURCE}" "${SYMBOLIC_DEST}"
    ICON_CHANGED=1
    CHANGED=1
fi
if [ "${ICON_CHANGED}" = "1" ] && command -v gtk-update-icon-cache >/dev/null 2>&1; then
    gtk-update-icon-cache -f -t "${ICON_ROOT}" >/dev/null 2>&1 || true
fi
UNIT_DEST="${HOME}/.config/systemd/user/hgs-tray.service"
mkdir -p "${HOME}/.config/systemd/user"
# %h -- специфик systemd (см. systemd.unit(5)), разворачивается самим systemd при
# старте юнита, копируем как есть.
if ! cmp -s "${SCRIPT_DIR}/packaging/hgs-tray.service" "${UNIT_DEST}" 2>/dev/null; then
    install -Dm644 "${SCRIPT_DIR}/packaging/hgs-tray.service" "${UNIT_DEST}"
    CHANGED=1
fi

DESKTOP_DEST="${HOME}/.local/share/applications/hgs-tray.desktop"
mkdir -p "${HOME}/.local/share/applications"
# В отличие от systemd, ключ Exec= в .desktop-файле полей спецификации вида %h не знает
# (desktop-file-validate отвергает его как invalid field code) -- нужен реальный путь,
# подстановкой пути в Exec=. Это НЕ
# автозапуск: файл ставится в ~/.local/share/applications (пункт меню приложений), а не
# в ~/.config/autostart, так что X-GNOME-Autostart-enabled/X-KDE-autostart-phase здесь
# ни на что не влияют -- единственный автозапуск ниже, через systemd --user.
# Resolve Icon to its actual file as well: Plasma's task icon renderer may
# substitute a generic executable icon for a newly installed local theme name.
# An absolute SVG path also works for the launcher and survives theme changes.
NEW_DESKTOP="$(sed -e "s|^Exec=.*|Exec=${HOME}/.local/bin/hgs-tray --sessions|" \
                  -e "s|^Icon=.*|Icon=${SYMBOLIC_DEST}|" "${SCRIPT_DIR}/packaging/hgs-tray.desktop")"
if [ "$(cat "${DESKTOP_DEST}" 2>/dev/null || true)" != "${NEW_DESKTOP}" ]; then
    printf '%s\n' "${NEW_DESKTOP}" > "${DESKTOP_DEST}"
    DESKTOP_CHANGED=1
    CHANGED=1
fi

# Plasma keeps its own application cache in addition to the GTK icon cache.
# Refresh it after changing either the launcher or its icon assets.
if { [ "${ICON_CHANGED}" = "1" ] || [ "${DESKTOP_CHANGED}" = "1" ]; } && command -v kbuildsycoca6 >/dev/null 2>&1; then
    kbuildsycoca6 --noincremental >/dev/null 2>&1 || true
fi

# --- 4. enable + start ----------------------------------------------------------
echo "[4/4] Reloading and enabling hgs-tray.service..."
systemctl --user daemon-reload
if [ "$(systemctl --user is-enabled hgs-tray.service 2>/dev/null || true)" != "enabled" ]; then
    CHANGED=1
fi
if [ "$(systemctl --user is-active hgs-tray.service 2>/dev/null || true)" != "active" ]; then
    CHANGED=1
fi
if [ "${UPDATE}" = "1" ]; then
    # Quits a running GUI, swaps the binary and starts the unit again.
    python3 "${SCRIPT_DIR}/update-installed.py" "${SCRIPT_DIR}/build/hgs-tray" "${BIN_DEST}"
fi
# enable --now: start -- не restart, поэтому уже активный трей второй раз не дёргает.
systemctl --user enable --now hgs-tray.service

HINT_SELFTEST="Check without a display:  ~/.local/bin/hgs-tray --selftest"
HINT_STATUS="Unit status:              systemctl --user status hgs-tray.service"

fi

if [ "${RELOAD}" = "1" ] && [ "${RESTART_RUNNING}" = "0" ]; then
    echo "Updated on disk. The running Zerus keeps its drafts; the new UI opens on next launch."
fi

echo
if [ "${CHANGED}" = "1" ]; then
    echo "Installed hgs-tray."
else
    echo "hgs-tray is already installed and unchanged."
fi
echo
echo "${HINT_SELFTEST}"
echo "${HINT_STATUS}"
# Только у маковской ветки: на Linux stderr юнита и так в журнале.
if [ -n "${HINT_LOG:-}" ]; then echo "${HINT_LOG}"; fi
