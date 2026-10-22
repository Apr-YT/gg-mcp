#!/system/bin/sh
cd /data/local/tmp/ggmcp
chmod 755 gg-agentd

# 加载KO模块（文件名带内核版本后缀，逐个加载而不是写死名字）
for m in ko/*.ko; do
    [ -f "$m" ] && insmod "$m" 2>/dev/null
done

# 启动agentd
./gg-agentd --driver rwproc --port 34127 &
echo "Agentd started"
