
#!/system/bin/sh
# GG-MCP 部署脚本

echo "=== GG-MCP 设备端部署 ==="

# 检查root权限
if [ "$(id -u)" != "0" ]; then
    echo "需要root权限"
    exit 1
fi

echo "[1/3] 设置执行权限..."
chmod 755 /data/local/tmp/ggmcp/gg-agentd
if [ $? -eq 0 ]; then
    echo "权限设置成功"
else
    echo "权限设置失败"
    exit 1
fi

echo "[2/3] 重启agentd..."
sh /data/local/tmp/ggmcp/start_agentd.sh restart
sleep 2

echo "[3/3] 验证运行状态..."
ps -A | grep gg-agentd

echo "=== 部署完成 ==="
