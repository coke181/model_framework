#!/usr/bin/env bash

# 文件职责：安全应用 Nginx 连接复用和 worker 连接数配置。
# 调用流程：备份当前配置 -> 安装候选站点配置 -> 调整 worker_connections -> nginx -t -> reload。
# 如果配置校验失败，会自动恢复站点配置和主配置，不会 reload 错误配置。

set -euo pipefail

candidate="${1:?请传入候选 Nginx server 配置路径}"
target="/etc/nginx/sites-enabled/ue-game-backend"
stamp="$(date +%Y%m%d-%H%M%S)"
backup_dir="/etc/nginx/backups"
site_backup="${backup_dir}/ue-game-backend.${stamp}.bak"
main_backup="/etc/nginx/nginx.conf.${stamp}.bak"

mkdir -p "${backup_dir}"
cp -a "${target}" "${site_backup}"
cp -a /etc/nginx/nginx.conf "${main_backup}"
install -o root -g root -m 0644 "${candidate}" "${target}"

# worker_connections 位于 nginx.conf 的 events 段，不能放进 sites-enabled。
perl -0pi -e 's/worker_connections\s+\d+;/worker_connections 8192;/' /etc/nginx/nginx.conf

if ! nginx -t; then
    cp -a "${site_backup}" "${target}"
    cp -a "${main_backup}" /etc/nginx/nginx.conf
    nginx -t
    echo "Nginx 配置校验失败，已恢复旧配置。" >&2
    exit 1
fi

systemctl reload nginx
echo "Nginx 优化配置已生效。"
echo "站点配置备份：${site_backup}"
echo "主配置备份：${main_backup}"
