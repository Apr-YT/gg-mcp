#!/system/bin/sh
cd /data/local/tmp/ggmcp
chmod 755 gg-agentd

# 加载KO模块
insmod ko/rwProcMem_module.ko 2>/dev/null
insmod ko/hwBreakpointProc_module.ko 2>/dev/null

# 启动agentd
./gg-agentd --driver rwproc --port 34127 &
echo "Agentd started"
