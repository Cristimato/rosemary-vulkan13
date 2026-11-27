// HUD/telemetria do vkshim (v4.9). Veja hud.h.
//
// Tempo de quadro: intervalo entre chamadas de vkQueueSignalReleaseImageANDROID (o loader chama
// uma por apresentação). Tempo de GPU: em cada vkQueueSubmit entram dois command buffers nossos,
// um no começo (reset + timestamp TOP_OF_PIPE) e outro no fim (timestamp BOTTOM_OF_PIPE). Os
// resultados são lidos sem esperar, no quadro seguinte; um slot só é reusado depois de lido. Se
// o anel estiver cheio, aquele submit fica sem medição (nunca bloqueia o jogo).
#include "hud.h"
#include <android/log.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "vkshim", __VA_ARGS__)
#define RING 32
#define MAXQ 8
#define MAXFT 2048

int hud_on;
volatile unsigned hud_draws;

typedef struct {
    VkQueue q; uint32_t fam; int ok, tried;
    VkCommandPool pool; VkQueryPool qp;
    VkCommandBuffer cs[RING], ce[RING];
    uint8_t st[RING]; uint32_t next;
} HQ;

static struct {
    VkDevice d; float period; uint64_t mask[16]; uint32_t nfam;
    PFN_vkCreateCommandPool ccp; PFN_vkDestroyCommandPool dcp;
    PFN_vkAllocateCommandBuffers acb; PFN_vkBeginCommandBuffer bcb; PFN_vkEndCommandBuffer ecb;
    PFN_vkCreateQueryPool cqp; PFN_vkDestroyQueryPool dqp; PFN_vkGetQueryPoolResults gqpr;
    PFN_vkCmdResetQueryPool crqp; PFN_vkCmdWriteTimestamp cwt;
    HQ q[MAXQ]; int nq;
} D;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;

static FILE *csv;
static char csv_path[300];
static long long t_start, sec_start, last_present;
static unsigned frames, submits, pipes, nft, lines;
static long long pipe_ns, ft[MAXFT];
/* latência de cada submit (do começo ao fim na fila da GPU), em ns. O contador de timestamp da
 * r49 na G76 não é contínuo entre submits (não serve para somar/unir intervalos de submits
 * diferentes); dentro do mesmo submit a diferença vale. */
#define MAXIV 1024
static long long lat[MAXIV];
static unsigned nlat;
long long hud_wait_ns;
static char thermal[96];
/* acumulado para o resumo de 5 s */
static double s5_fps, s5_ft, s5_gpu, s5_max; static unsigned s5_n;

static long long now_ns(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000000000LL + t.tv_nsec;
}

static int read_small(const char *p, char *buf, size_t n) {
    FILE *f = fopen(p, "r");
    if (!f) return -1;
    size_t r = fread(buf, 1, n - 1, f);
    fclose(f);
    buf[r] = 0;
    return (int)r;
}

void hud_init(const char *pkg) {
    if (!hud_on || csv) return;
    if (pkg[0] && !strchr(pkg, '/')) {
        snprintf(csv_path, sizeof csv_path, "/storage/emulated/0/Android/data/%s/vkshim_hud.csv", pkg);
        csv = fopen(csv_path, "w");
        if (!csv) {
            snprintf(csv_path, sizeof csv_path, "/data/user/0/%s/cache/vkshim_hud.csv", pkg);
            csv = fopen(csv_path, "w");
        }
    } else {
        const char *td = getenv("VKSHIM_HUD_FILE");
        if (td) { snprintf(csv_path, sizeof csv_path, "%s", td); csv = fopen(csv_path, "w"); }
    }
    if (csv) {
        fprintf(csv, "t_s,fps,ft_med_ms,ft_p95_ms,ft_max_ms,espera_ms,lat_gpu_ms,submits_quadro,draws_quadro,pipes_novos,pipe_ms,gpu_mhz,gpu_uso_pct,temp_c\n");
        fflush(csv);
        LOGI("hud ligado: %s", csv_path);
    } else LOGI("hud: não consegui criar o CSV (só logcat)");
    /* zona térmica da CPU/SoC (a G90T não tem zona só da GPU) */
    for (int i = 0; i < 40 && !thermal[0]; i++) {
        char p[96], t[32];
        snprintf(p, sizeof p, "/sys/class/thermal/thermal_zone%d/type", i);
        if (read_small(p, t, sizeof t) <= 0) continue;
        if (!strncmp(t, "mtktscpu", 8)) snprintf(thermal, sizeof thermal, "/sys/class/thermal/thermal_zone%d/temp", i);
    }
    t_start = sec_start = now_ns();
}

#define GET(field, name) D.field = (void *)gdpa(d, name)
void hud_device(VkDevice d, VkPhysicalDevice pd, VkInstance inst, PFN_vkGetDeviceProcAddr gdpa, PFN_vkGetInstanceProcAddr gipa) {
    if (!hud_on) return;
    pthread_mutex_lock(&mu);
    if (D.d) { pthread_mutex_unlock(&mu); return; }      /* só o primeiro device */
    memset(&D, 0, sizeof D);
    D.d = d;
    GET(ccp, "vkCreateCommandPool"); GET(dcp, "vkDestroyCommandPool"); GET(acb, "vkAllocateCommandBuffers");
    GET(bcb, "vkBeginCommandBuffer"); GET(ecb, "vkEndCommandBuffer"); GET(cqp, "vkCreateQueryPool");
    GET(dqp, "vkDestroyQueryPool"); GET(gqpr, "vkGetQueryPoolResults"); GET(crqp, "vkCmdResetQueryPool");
    GET(cwt, "vkCmdWriteTimestamp");
    PFN_vkGetPhysicalDeviceProperties gp = (void *)gipa(inst, "vkGetPhysicalDeviceProperties");
    PFN_vkGetPhysicalDeviceQueueFamilyProperties gq = (void *)gipa(inst, "vkGetPhysicalDeviceQueueFamilyProperties");
    if (gp) { VkPhysicalDeviceProperties pr; gp(pd, &pr); D.period = pr.limits.timestampPeriod; }
    if (gq) {
        uint32_t n = 0; gq(pd, &n, NULL);
        if (n > 16) n = 16;
        VkQueueFamilyProperties fp[16]; gq(pd, &n, fp);
        D.nfam = n;
        for (uint32_t i = 0; i < n; i++) {
            uint32_t b = fp[i].timestampValidBits;
            D.mask[i] = b >= 64 ? ~0ull : b ? ((1ull << b) - 1) : 0;
        }
    }
    pthread_mutex_unlock(&mu);
    LOGI("hud: timestampPeriod=%.2f ns, bits família 0 = %s", D.period, D.mask[0] ? "ok" : "0 (sem tempo de GPU)");
}

void hud_device_destroy(VkDevice d) {
    pthread_mutex_lock(&mu);
    if (D.d == d) {
        for (int i = 0; i < D.nq; i++) {
            if (D.q[i].pool) D.dcp(d, D.q[i].pool, NULL);
            if (D.q[i].qp) D.dqp(d, D.q[i].qp, NULL);
        }
        memset(&D, 0, sizeof D);
    }
    pthread_mutex_unlock(&mu);
    if (csv) fflush(csv);
}

void hud_queue(VkDevice d, VkQueue q, uint32_t family) {
    if (!hud_on || !q) return;
    pthread_mutex_lock(&mu);
    if (D.d == d) {
        int found = 0;
        for (int i = 0; i < D.nq; i++) if (D.q[i].q == q) found = 1;
        if (!found && D.nq < MAXQ) { memset(&D.q[D.nq], 0, sizeof D.q[0]); D.q[D.nq].q = q; D.q[D.nq].fam = family; D.nq++; }
    }
    pthread_mutex_unlock(&mu);
}

/* cria pool, query pool e os pares de command buffers (gravados uma vez, reusados) */
static int q_setup(HQ *h) {
    h->tried = 1;
    if (!D.ccp || !D.cqp || !D.cwt || h->fam >= D.nfam || !D.mask[h->fam]) return 0;
    VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, 0, 0, h->fam };
    if (D.ccp(D.d, &pci, NULL, &h->pool)) return 0;
    VkQueryPoolCreateInfo qci = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, 0, 0, VK_QUERY_TYPE_TIMESTAMP, 2 * RING, 0 };
    if (D.cqp(D.d, &qci, NULL, &h->qp)) return 0;
    VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, 0, h->pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, RING };
    if (D.acb(D.d, &ai, h->cs) || D.acb(D.d, &ai, h->ce)) return 0;
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    for (int i = 0; i < RING; i++) {
        /* o loader espera o ponteiro de dispatch nos objetos dele; estes nunca passam pelo loader */
        if (D.bcb(h->cs[i], &bi)) return 0;
        D.crqp(h->cs[i], h->qp, 2 * i, 2);
        D.cwt(h->cs[i], VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, h->qp, 2 * i);
        if (D.ecb(h->cs[i])) return 0;
        if (D.bcb(h->ce[i], &bi)) return 0;
        D.cwt(h->ce[i], VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, h->qp, 2 * i + 1);
        if (D.ecb(h->ce[i])) return 0;
    }
    h->ok = 1;
    return 1;
}

/* lê o slot se já terminou; 1 = lido (livre), 0 = ainda na GPU */
static int harvest(HQ *h, int i) {
    uint64_t t[2];
    if (D.gqpr(D.d, h->qp, 2 * i, 2, sizeof t, t, 8, VK_QUERY_RESULT_64_BIT) != VK_SUCCESS) return 0;
    uint64_t m = D.mask[h->fam], dt = (t[1] - t[0]) & m;
    if (dt < (uint64_t)(2e9 / (D.period > 0 ? D.period : 1)) && nlat < MAXIV) lat[nlat++] = (long long)(dt * D.period);
    h->st[i] = 0;
    return 1;
}

static int take_slot(VkQueue q, HQ **out) {
    int slot = -1;
    pthread_mutex_lock(&mu);
    for (int k = 0; k < D.nq; k++) {
        HQ *h = &D.q[k];
        if (h->q != q) continue;
        if (!h->tried) q_setup(h);
        if (!h->ok) break;
        int i = (int)h->next;
        if (h->st[i] && !harvest(h, i)) break;
        h->st[i] = 1; h->next = (h->next + 1) % RING;
        slot = i; *out = h;
        break;
    }
    pthread_mutex_unlock(&mu);
    return slot;
}
static void drop_slot(HQ *h, int i) { pthread_mutex_lock(&mu); h->st[i] = 0; pthread_mutex_unlock(&mu); }

VkResult hud_submit(VkQueue q, uint32_t n, const VkSubmitInfo *s, VkFence f, PFN_vkQueueSubmit real) {
    __sync_fetch_and_add(&submits, 1);
    int first = -1, last = -1;
    for (uint32_t i = 0; i < n; i++) if (s[i].commandBufferCount) { if (first < 0) first = (int)i; last = (int)i; }
    HQ *h = NULL;
    int slot = first >= 0 ? take_slot(q, &h) : -1;
    if (slot < 0) return real(q, n, s, f);
    VkSubmitInfo *c = malloc(n * sizeof *c);
    VkCommandBuffer *a = malloc((s[first].commandBufferCount + 1) * sizeof *a);
    VkCommandBuffer *b = last != first ? malloc((s[last].commandBufferCount + 1) * sizeof *b) : NULL;
    if (!c || !a || (last != first && !b)) { free(c); free(a); free(b); drop_slot(h, slot); return real(q, n, s, f); }
    memcpy(c, s, n * sizeof *c);
    if (first == last) {
        uint32_t k = s[first].commandBufferCount;
        a = realloc(a, (k + 2) * sizeof *a);
        a[0] = h->cs[slot]; memcpy(&a[1], s[first].pCommandBuffers, k * sizeof *a); a[k + 1] = h->ce[slot];
        c[first].pCommandBuffers = a; c[first].commandBufferCount = k + 2;
    } else {
        uint32_t k = s[first].commandBufferCount, m = s[last].commandBufferCount;
        a[0] = h->cs[slot]; memcpy(&a[1], s[first].pCommandBuffers, k * sizeof *a);
        c[first].pCommandBuffers = a; c[first].commandBufferCount = k + 1;
        memcpy(b, s[last].pCommandBuffers, m * sizeof *b); b[m] = h->ce[slot];
        c[last].pCommandBuffers = b; c[last].commandBufferCount = m + 1;
    }
    VkResult r = real(q, n, c, f);
    if (r != VK_SUCCESS) drop_slot(h, slot);
    free(c); free(a); free(b);
    return r;
}

VkResult hud_submit2(VkQueue q, uint32_t n, const VkSubmitInfo2 *s, VkFence f, PFN_vkQueueSubmit2 real) {
    __sync_fetch_and_add(&submits, 1);
    int first = -1, last = -1;
    for (uint32_t i = 0; i < n; i++) if (s[i].commandBufferInfoCount) { if (first < 0) first = (int)i; last = (int)i; }
    HQ *h = NULL;
    int slot = first >= 0 ? take_slot(q, &h) : -1;
    if (slot < 0) return real(q, n, s, f);
    uint32_t k = s[first].commandBufferInfoCount, m = s[last].commandBufferInfoCount;
    VkSubmitInfo2 *c = malloc(n * sizeof *c);
    VkCommandBufferSubmitInfo *a = malloc((k + 2) * sizeof *a), *b = last != first ? malloc((m + 1) * sizeof *b) : NULL;
    if (!c || !a || (last != first && !b)) { free(c); free(a); free(b); drop_slot(h, slot); return real(q, n, s, f); }
    memcpy(c, s, n * sizeof *c);
    VkCommandBufferSubmitInfo cs = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, 0, h->cs[slot], 0 };
    VkCommandBufferSubmitInfo ce = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, 0, h->ce[slot], 0 };
    /* device mask igual ao do app (0 = todos) */
    if (k) cs.deviceMask = s[first].pCommandBufferInfos[0].deviceMask;
    if (m) ce.deviceMask = s[last].pCommandBufferInfos[0].deviceMask;
    a[0] = cs; memcpy(&a[1], s[first].pCommandBufferInfos, k * sizeof *a);
    if (first == last) { a[k + 1] = ce; c[first].pCommandBufferInfos = a; c[first].commandBufferInfoCount = k + 2; }
    else {
        c[first].pCommandBufferInfos = a; c[first].commandBufferInfoCount = k + 1;
        memcpy(b, s[last].pCommandBufferInfos, m * sizeof *b); b[m] = ce;
        c[last].pCommandBufferInfos = b; c[last].commandBufferInfoCount = m + 1;
    }
    VkResult r = real(q, n, c, f);
    if (r != VK_SUCCESS) drop_slot(h, slot);
    free(c); free(a); free(b);
    return r;
}

void hud_pipe(long long ns, unsigned n) { __sync_fetch_and_add(&pipe_ns, ns); __sync_fetch_and_add(&pipes, n); }

static int cmp_ll(const void *a, const void *b) { long long x = *(const long long *)a, y = *(const long long *)b; return x < y ? -1 : x > y; }

static void emit_second(long long now) {
    double el = (now - sec_start) / 1e9;
    /* colhe o que já terminou */
    pthread_mutex_lock(&mu);
    for (int k = 0; k < D.nq; k++) {
        HQ *h = &D.q[k];
        if (!h->ok) continue;
        for (int i = 0; i < RING; i++) if (h->st[i]) harvest(h, i);
    }
    double lmed = -1;
    if (nlat) { qsort(lat, nlat, sizeof *lat, cmp_ll); lmed = lat[nlat / 2] / 1e6; }
    nlat = 0;
    long long wns = hud_wait_ns; hud_wait_ns = 0;
    pthread_mutex_unlock(&mu);
    unsigned fr = frames ? frames : 1;
    double fps = frames / el, ftm = 0, p95 = 0, mx = 0;
    if (nft) {
        qsort(ft, nft, sizeof *ft, cmp_ll);
        ftm = ft[nft / 2] / 1e6; p95 = ft[(nft * 95) / 100 < nft ? (nft * 95) / 100 : nft - 1] / 1e6; mx = ft[nft - 1] / 1e6;
    }
    double esp = wns / 1e6 / fr;
    char b[64]; int mhz = -1, uso = -1; double tc = -1;
    if (read_small("/sys/kernel/ged/hal/current_freqency", b, sizeof b) > 0) { int i_, k_; if (sscanf(b, "%d %d", &i_, &k_) == 2) mhz = k_ / 1000; }
    if (read_small("/sys/kernel/ged/hal/gpu_utilization", b, sizeof b) > 0) { int a_; if (sscanf(b, "%d", &a_) == 1) uso = a_; }
    if (thermal[0] && read_small(thermal, b, sizeof b) > 0) tc = atoi(b) / 1000.0;
    if (csv) {
        fprintf(csv, "%.0f,%.1f,%.2f,%.2f,%.2f,%.2f,%.2f,%.1f,%.0f,%u,%.1f,%d,%d,%.1f\n",
                (now - t_start) / 1e9, fps, ftm, p95, mx, esp, lmed, (double)submits / fr, (double)hud_draws / fr,
                pipes, pipe_ns / 1e6, mhz, uso, tc);
        fflush(csv);
    }
    s5_fps += fps; s5_ft += ftm; s5_gpu += esp; if (mx > s5_max) s5_max = mx; s5_n++;
    if (++lines % 5 == 0 && s5_n) {
        LOGI("hud: %.1f fps | quadro %.1f ms (pior %.1f) | esperando a GPU %.1f ms/quadro | %.0f draws | %u pipes novos | %d MHz uso %d%% | %.1f °C",
             s5_fps / s5_n, s5_ft / s5_n, s5_max, s5_gpu / s5_n, (double)hud_draws / fr, pipes, mhz, uso, tc);
        s5_fps = s5_ft = s5_gpu = s5_max = 0; s5_n = 0;
    }
    frames = submits = pipes = nft = 0; pipe_ns = 0; hud_draws = 0;
    sec_start = now;
}

void hud_present(void) {
    long long now = now_ns();
    if (last_present && nft < MAXFT) ft[nft++] = now - last_present;
    last_present = now;
    frames++;
    if (now - sec_start >= 1000000000LL) emit_second(now);
}
