#!/usr/bin/env bash
# 端到端 RTSP 验收：mediamtx 提供服务，ffmpeg 推流，pipeline_smoke 拉流校验。
#
# 用法:
#   tests/run_rtsp_test.sh [视频文件] [期望帧数]
#
# 环境变量（都有默认值）:
#   MEDIAMTX  mediamtx 可执行文件路径
#   FFMPEG    ffmpeg 可执行文件路径
#   VD_RTSP_URL  推流/拉流地址（默认 rtsp://127.0.0.1:8554/live）
#   VD_BUILD_DIR 构建目录（默认 <repo>/build）
#
# 说明: 拉流端只做拉流+解码（不依赖 GUI），因此本脚本在无 OpenGL 的环境同样可跑。

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${VD_BUILD_DIR:-${ROOT_DIR}/build}"
RTSP_URL="${VD_RTSP_URL:-rtsp://127.0.0.1:8554/live}"
VIDEO_FILE="${1:-}"
EXPECTED_FRAMES="${2:-120}"

# mediamtx 优先取 ~/tools/mediamtx（独立安装位置），其次取本仓库 build-tools，最后取 PATH。
MEDIAMTX="${MEDIAMTX:-${HOME}/tools/mediamtx/mediamtx}"
if [[ ! -x "${MEDIAMTX}" ]]; then
    MEDIAMTX="${ROOT_DIR}/build-tools/mediamtx"
fi
if [[ ! -x "${MEDIAMTX}" ]]; then
    MEDIAMTX="$(command -v mediamtx || true)"
fi
if [[ -z "${MEDIAMTX}" || ! -x "${MEDIAMTX}" ]]; then
    echo "[rtsp] 找不到 mediamtx：请设置 MEDIAMTX=/path/to/mediamtx" >&2
    exit 2
fi

# 必须带配置文件启动：mediamtx 无配置时"路径未配置"，会直接拒绝推流（400 Bad Request）。
MEDIAMTX_CONF="${MEDIAMTX_CONF:-$(dirname "${MEDIAMTX}")/mediamtx.yml}"

FFMPEG="${FFMPEG:-/home/fox/workspace/FFmpeg/install/ffmpeg7-shared/bin/ffmpeg}"
if [[ ! -x "${FFMPEG}" ]]; then
    FFMPEG="$(command -v ffmpeg || true)"
fi
if [[ -z "${FFMPEG}" || ! -x "${FFMPEG}" ]]; then
    echo "[rtsp] 找不到 ffmpeg：请设置 FFMPEG=/path/to/ffmpeg" >&2
    exit 2
fi

SMOKE="${BUILD_DIR}/tests/pipeline_smoke"
if [[ ! -x "${SMOKE}" ]]; then
    echo "[rtsp] 找不到 ${SMOKE}：请先构建（cmake --build ${BUILD_DIR}）" >&2
    exit 2
fi

if [[ -z "${VIDEO_FILE}" ]]; then
    echo "[rtsp] 用法: $0 [视频文件] [期望帧数]" >&2
    exit 2
fi
if [[ ! -f "${VIDEO_FILE}" ]]; then
    echo "[rtsp] 视频文件不存在: ${VIDEO_FILE}" >&2
    exit 2
fi

WORK_DIR="${BUILD_DIR}/rtsp"
mkdir -p "${WORK_DIR}"

cleanup() {
    [[ -n "${PUSH_PID:-}" ]] && kill "${PUSH_PID}" 2>/dev/null || true
    [[ -n "${SERVER_PID:-}" ]] && kill "${SERVER_PID}" 2>/dev/null || true
    wait 2>/dev/null || true
}
trap cleanup EXIT

echo "[rtsp] 启动 mediamtx: ${MEDIAMTX} ${MEDIAMTX_CONF}"
if [[ -f "${MEDIAMTX_CONF}" ]]; then
    (cd "${WORK_DIR}" && "${MEDIAMTX}" "${MEDIAMTX_CONF}" > "${WORK_DIR}/mediamtx.log" 2>&1) &
else
    echo "[rtsp] 警告: 未找到 ${MEDIAMTX_CONF}，mediamtx 将使用空配置（推流可能被拒）" >&2
    (cd "${WORK_DIR}" && "${MEDIAMTX}" > "${WORK_DIR}/mediamtx.log" 2>&1) &
fi
SERVER_PID=$!
sleep 1

echo "[rtsp] 推流: ${VIDEO_FILE} -> ${RTSP_URL}"
"${FFMPEG}" -hide_banner -loglevel warning -re -stream_loop -1 -i "${VIDEO_FILE}" \
    -c copy -f rtsp -rtsp_transport tcp "${RTSP_URL}" > "${WORK_DIR}/push.log" 2>&1 &
PUSH_PID=$!
sleep 3

if ! kill -0 "${PUSH_PID}" 2>/dev/null; then
    echo "[rtsp] 推流进程已退出，日志:" >&2
    tail -20 "${WORK_DIR}/push.log" >&2
    exit 1
fi

echo "[rtsp] 拉流校验: ${RTSP_URL} (期望 >= ${EXPECTED_FRAMES} 帧)"
set +e
"${SMOKE}" "${RTSP_URL}" --frames "${EXPECTED_FRAMES}" --timeout-ms 60000
SMOKE_RC=$?
set -e

if [[ ${SMOKE_RC} -eq 0 ]]; then
    echo "[rtsp] 通过"
else
    echo "[rtsp] 未通过（退出码 ${SMOKE_RC}）；推流日志:" >&2
    tail -20 "${WORK_DIR}/push.log" >&2
fi
exit "${SMOKE_RC}"
