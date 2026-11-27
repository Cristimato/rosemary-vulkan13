#!/system/bin/sh
# vkshim v4.9: deixa o HUD ler frequência/uso da GPU (os arquivos do ged são 0440 root)
for i in $(seq 1 30); do [ -e /sys/kernel/ged/hal/current_freqency ] && break; sleep 2; done
chmod 0444 /sys/kernel/ged/hal/current_freqency /sys/kernel/ged/hal/gpu_utilization 2>/dev/null
