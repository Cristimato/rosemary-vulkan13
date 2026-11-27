// spvemu: emulações feitas por reescrita de SPIR-V (vkshim v4.7).
//
//  ClipDistance  A Mali-G76 não tem planos de recorte (maxClipDistances = 0). No último estágio
//                antes da rasterização o gl_ClipDistance de saída é redirecionado para uma
//                variável Private (iniciada em 1.0) e copiado, a cada fim de vértice, para
//                varyings vec4 numa location livre. O fragment shader lê essas varyings e
//                descarta o fragmento quando alguma distância é negativa. Para distâncias que
//                variam linearmente na primitiva (planos), é o mesmo conjunto de pixels que o
//                recorte do hardware manteria. O built-in original fica sem uso e o clipcull o
//                retira depois.
//  depthClamp    z de gl_Position preso a [0, w] em cada vértice: a primitiva deixa de ser cortada
//                pelos planos near/far e a profundidade fica dentro do intervalo do viewport.
//                É exato quando toda a primitiva está do mesmo lado do plano; numa primitiva que
//                cruza o plano a profundidade dos vértices de fora é achatada (como no "shadow
//                pancaking" das engines) em vez de grampeada por pixel.
//  gl_DrawID     Com o multiDrawIndirect convertido em N draws, cada draw é o nº 0. As leituras de
//                gl_DrawID passam a vir de um push constant que o vkshim grava antes de cada draw.
//
// Qualquer uso não reconhecido devolve NULL: o chamador segue sem a emulação (comportamento da v4).
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "spvemu.h"

#define OP(w) ((w) & 0xffff)
#define WC(w) ((w) >> 16)
#define MAXB (1u << 22)

enum {
    SC_INPUT = 1, SC_OUTPUT = 3, SC_PRIVATE = 6, SC_PUSH = 9,
    D_BLOCK = 2, D_BUILTIN = 11, D_LOCATION = 30, D_OFFSET = 35,
    B_POSITION = 0, B_CLIP = 3, B_DRAWINDEX = 4426,
};

/* ------------------------------------------------------------------ buffer de palavras */
typedef struct { uint32_t *w; size_t n, cap; } Buf;
static int oom;
static void bput(Buf *b, uint32_t v) {
    if (b->n == b->cap) {
        size_t c = b->cap ? b->cap * 2 : 32;
        uint32_t *p = realloc(b->w, c * 4);
        if (!p) { oom = 1; return; }
        b->w = p; b->cap = c;
    }
    b->w[b->n++] = v;
}
static void binst(Buf *b, uint32_t op, const uint32_t *a, uint32_t na) {
    bput(b, ((na + 1) << 16) | op);
    for (uint32_t i = 0; i < na; i++) bput(b, a[i]);
}
#define EMIT(b, op, ...) do { const uint32_t _a[] = { __VA_ARGS__ }; binst((b), (op), _a, sizeof _a / 4); } while (0)

/* ------------------------------------------------------------------ editor */
typedef struct {
    const uint32_t *in; size_t nw;
    uint32_t *at; uint32_t ni;      /* deslocamento de cada instrução */
    Buf *pre;                       /* palavras inseridas antes da instrução i (pre[ni] = fim) */
    Buf *rep; uint8_t *has_rep;     /* substituição da instrução i (vazia = remover) */
    uint32_t bound, version, bound0;
    uint32_t *def;                  /* id -> índice da instrução que o define (tipos, constantes, variáveis) */
    uint32_t i_annot_end;           /* onde entram decorações novas */
    uint32_t i_func;                /* primeira OpFunction (tipos/variáveis novos entram antes) */
    Buf added;                      /* tipos/constantes novos: [op, n, args..., id] para reuso */
    /* entry point escolhido */
    uint32_t ep_i, ep_model, ep_fn, ep_iface;   /* ep_iface = 1ª palavra da interface */
    uint32_t fn_begin, fn_end;      /* índices das instruções da função do entry point */
} Ed;

static const uint32_t *I(const Ed *e, uint32_t i) { return &e->in[e->at[i]]; }

static int preamble_op(uint32_t op) {
    switch (op) {
    case 0: case 2: case 3: case 4: case 5: case 6: case 7: case 10: case 11: case 14: case 15:
    case 16: case 17: case 71: case 72: case 73: case 74: case 75: case 330: case 331: case 332:
    case 5632: case 5633: case 8: case 317:
        return 1;
    }
    return 0;
}

static void ed_free(Ed *e) {
    if (e->pre) for (uint32_t i = 0; i <= e->ni; i++) free(e->pre[i].w);
    if (e->rep) for (uint32_t i = 0; i < e->ni; i++) free(e->rep[i].w);
    free(e->at); free(e->pre); free(e->rep); free(e->has_rep); free(e->def); free(e->added.w);
    memset(e, 0, sizeof *e);
}

static int ed_init(Ed *e, const uint32_t *in, size_t nw, const char *entry) {
    memset(e, 0, sizeof *e);
    if (nw < 5 || in[0] != 0x07230203 || in[3] > MAXB) return -1;
    e->in = in; e->nw = nw; e->bound = e->bound0 = in[3]; e->version = in[1];
    uint32_t ni = 0;
    for (size_t i = 5; i < nw;) { uint32_t wc = WC(in[i]); if (!wc || i + wc > nw) return -1; i += wc; ni++; }
    e->ni = ni;
    e->at = malloc((ni + 1) * 4);
    e->pre = calloc(ni + 1, sizeof(Buf));
    e->rep = calloc(ni, sizeof(Buf));
    e->has_rep = calloc(ni, 1);
    e->def = calloc(e->bound0, 4);
    if (!e->at || !e->pre || !e->rep || !e->has_rep || !e->def) { ed_free(e); return -1; }
    uint32_t k = 0, annot_end = 0, have_annot = 0;
    e->i_func = ni; e->ep_i = ni;
    size_t elen = entry ? strlen(entry) : 0;
    for (size_t i = 5; i < nw; i += WC(in[i]), k++) {
        e->at[k] = (uint32_t)i;
        const uint32_t *w = &in[i]; uint32_t op = OP(w[0]), wc = WC(w[0]);
        if (op >= 19 && op <= 39 && wc >= 2 && w[1] < e->bound) e->def[w[1]] = k;
        if (((op >= 41 && op <= 52) || op == 59) && wc >= 3 && w[2] < e->bound) e->def[w[2]] = k;
        if (op == 54 && e->i_func == ni) e->i_func = k;
        if (!have_annot && e->i_func == ni && !preamble_op(op)) { annot_end = k; have_annot = 1; }
        if (op == 15 && wc >= 4 && e->ep_i == ni) {
            const char *nm = (const char *)&w[3];
            size_t maxlen = (wc - 3) * 4;
            size_t l = strnlen(nm, maxlen);
            if (l < maxlen && (!entry || (l == elen && !memcmp(nm, entry, l)))) {
                e->ep_i = k; e->ep_model = w[1]; e->ep_fn = w[2];
                e->ep_iface = 3 + (uint32_t)(l / 4 + 1);
            }
        }
    }
    e->at[ni] = (uint32_t)nw;
    if (e->ep_i == ni || e->i_func == ni) { ed_free(e); return -1; }
    e->i_annot_end = have_annot ? annot_end : e->i_func;
    /* função do entry point */
    e->fn_begin = e->fn_end = ni;
    for (uint32_t i = e->i_func; i < ni; i++) {
        const uint32_t *w = I(e, i);
        if (OP(w[0]) == 54 && w[2] == e->ep_fn) e->fn_begin = i;
        if (OP(w[0]) == 56 && e->fn_begin != ni && e->fn_end == ni && i > e->fn_begin) { e->fn_end = i; break; }
    }
    if (e->fn_begin == ni || e->fn_end == ni) { ed_free(e); return -1; }
    return 0;
}

static uint32_t ed_id(Ed *e) { return e->bound++; }

/* devolve o id de um tipo/constante com estes operandos; cria antes da 1ª função se não existir.
 * res_at: posição do id de resultado (1 = tipos, 2 = constantes). */
static uint32_t ed_find_at(Ed *e, uint32_t op, int res_at, const uint32_t *a, uint32_t na, uint32_t at);
static uint32_t ed_find(Ed *e, uint32_t op, int res_at, const uint32_t *a, uint32_t na) { return ed_find_at(e, op, res_at, a, na, e->i_func); }
/* como ed_find, mas um tipo/constante novo é declarado antes da instrução 'at' */
static uint32_t ed_find_at(Ed *e, uint32_t op, int res_at, const uint32_t *a, uint32_t na, uint32_t at) {
    for (uint32_t i = 0; i < e->i_func; i++) {
        const uint32_t *w = I(e, i);
        if (OP(w[0]) != op || WC(w[0]) != na + 2) continue;
        if (res_at == 1 && !memcmp(&w[2], a, na * 4)) return w[1];
        if (res_at == 2 && w[1] == a[0] && !memcmp(&w[3], &a[1], (na - 1) * 4)) return w[2];
    }
    for (size_t p = 0; p < e->added.n;) {
        uint32_t aop = e->added.w[p], an = e->added.w[p + 1];
        if (aop == op && an == na && !memcmp(&e->added.w[p + 2], a, na * 4)) return e->added.w[p + 2 + an];
        p += 3 + an;
    }
    uint32_t id = ed_id(e);
    Buf *b = &e->pre[at];
    bput(b, ((na + 2) << 16) | op);
    if (res_at == 1) { bput(b, id); for (uint32_t i = 0; i < na; i++) bput(b, a[i]); }
    else { bput(b, a[0]); bput(b, id); for (uint32_t i = 1; i < na; i++) bput(b, a[i]); }
    bput(&e->added, op); bput(&e->added, na);
    for (uint32_t i = 0; i < na; i++) bput(&e->added, a[i]);
    bput(&e->added, id);
    return id;
}
static uint32_t t_float(Ed *e) { uint32_t a[] = { 32 }; return ed_find(e, 22, 1, a, 1); }
static uint32_t t_int(Ed *e) { uint32_t a[] = { 32, 1 }; return ed_find(e, 21, 1, a, 2); }
static uint32_t t_bool(Ed *e) { return ed_find(e, 20, 1, NULL, 0); }
static uint32_t t_vec4(Ed *e) { uint32_t a[] = { t_float(e), 4 }; return ed_find(e, 23, 1, a, 2); }
static uint32_t t_ptr(Ed *e, uint32_t sc, uint32_t t) { uint32_t a[] = { sc, t }; return ed_find(e, 32, 1, a, 2); }
static uint32_t c_int(Ed *e, int32_t v) { uint32_t a[] = { t_int(e), (uint32_t)v }; return ed_find(e, 43, 2, a, 2); }
static uint32_t c_float(Ed *e, float v) { uint32_t u; memcpy(&u, &v, 4); uint32_t a[] = { t_float(e), u }; return ed_find(e, 43, 2, a, 2); }

/* acrescenta ids à interface do entry point (reconstrói a instrução) */
static void ed_iface_add(Ed *e, const uint32_t *ids, uint32_t n) {
    Buf *r = &e->rep[e->ep_i];
    const uint32_t *w = I(e, e->ep_i);
    uint32_t wc = e->has_rep[e->ep_i] ? WC(r->w[0]) : WC(w[0]);
    if (!e->has_rep[e->ep_i]) { for (uint32_t k = 0; k < wc; k++) bput(r, w[k]); e->has_rep[e->ep_i] = 1; }
    for (uint32_t k = 0; k < n; k++) bput(r, ids[k]);
    if (!oom) r->w[0] = ((wc + n) << 16) | 15;
}
static void ed_set_rep(Ed *e, uint32_t i, const uint32_t *w, uint32_t n) {
    Buf *r = &e->rep[i];
    r->n = 0;
    for (uint32_t k = 0; k < n; k++) bput(r, w[k]);
    e->has_rep[i] = 1;
}

static uint32_t *ed_build(Ed *e, size_t *out_nw) {
    if (oom) return NULL;
    size_t total = 5;
    for (uint32_t i = 0; i <= e->ni; i++) total += e->pre[i].n;
    for (uint32_t i = 0; i < e->ni; i++) total += e->has_rep[i] ? e->rep[i].n : WC(I(e, i)[0]);
    uint32_t *out = malloc(total * 4);
    if (!out) return NULL;
    memcpy(out, e->in, 20);
    out[3] = e->bound;
    size_t o = 5;
    for (uint32_t i = 0; i <= e->ni; i++) {
        memcpy(&out[o], e->pre[i].w ? e->pre[i].w : out, e->pre[i].n * 4); o += e->pre[i].n;
        if (i == e->ni) break;
        if (e->has_rep[i]) { memcpy(&out[o], e->rep[i].w ? e->rep[i].w : out, e->rep[i].n * 4); o += e->rep[i].n; }
        else { uint32_t wc = WC(I(e, i)[0]); memcpy(&out[o], I(e, i), wc * 4); o += wc; }
    }
    *out_nw = o;
    return out;
}

/* ------------------------------------------------------------------ consultas */
static const uint32_t *defw(const Ed *e, uint32_t id) {
    if (id >= e->bound0 || !e->def[id]) return NULL;
    return I(e, e->def[id]);
}
static uint32_t ptr_pointee(const Ed *e, uint32_t ptr_t) {
    const uint32_t *w = defw(e, ptr_t);
    return (w && OP(w[0]) == 32) ? w[3] : 0;
}
static int32_t const_val(const Ed *e, uint32_t id, int *ok) {
    const uint32_t *w = defw(e, id);
    *ok = w && OP(w[0]) == 43 && WC(w[0]) == 4;
    return *ok ? (int32_t)w[3] : 0;
}
static int loc_count(const Ed *e, uint32_t t, int depth) {
    const uint32_t *w = defw(e, t);
    if (!w || depth > 8) return 1;
    int ok;
    switch (OP(w[0])) {
    case 23: { const uint32_t *c = defw(e, w[2]); int wide = c && WC(c[0]) >= 3 && c[2] == 64; return (wide && w[3] > 2) ? 2 : 1; }
    case 24: return (int)w[3] * loc_count(e, w[2], depth + 1);
    case 28: { int32_t n = const_val(e, w[3], &ok); return (ok && n > 0 ? n : 1) * loc_count(e, w[2], depth + 1); }
    case 30: { int s = 0; for (uint32_t k = 2; k < WC(w[0]); k++) s += loc_count(e, w[k], depth + 1); return s; }
    }
    return 1;
}

/* decoração BuiltIn de uma variável ou de um membro de struct */
static int var_builtin(const Ed *e, uint32_t id) {
    for (uint32_t i = 0; i < e->i_annot_end; i++) {
        const uint32_t *w = I(e, i);
        if (OP(w[0]) == 71 && WC(w[0]) >= 4 && w[1] == id && w[2] == D_BUILTIN) return (int)w[3];
    }
    return -1;
}
static int member_builtin(const Ed *e, uint32_t st, uint32_t builtin) {
    for (uint32_t i = 0; i < e->i_annot_end; i++) {
        const uint32_t *w = I(e, i);
        if (OP(w[0]) == 72 && WC(w[0]) >= 5 && w[1] == st && w[3] == D_BUILTIN && w[4] == builtin) return (int)w[2];
    }
    return -1;
}
static int var_location(const Ed *e, uint32_t id) {
    for (uint32_t i = 0; i < e->i_annot_end; i++) {
        const uint32_t *w = I(e, i);
        if (OP(w[0]) == 71 && WC(w[0]) >= 4 && w[1] == id && w[2] == D_LOCATION) return (int)w[3];
    }
    return -1;
}

/* fim (exclusivo) das locations usadas pelas variáveis de usuário com esta storage class */
static int loc_end(const Ed *e, uint32_t sc) {
    int end = 0;
    const uint32_t *ep = I(e, e->ep_i);
    for (uint32_t k = e->ep_iface; k < WC(ep[0]); k++) {
        const uint32_t *v = defw(e, ep[k]);
        if (!v || OP(v[0]) != 59 || v[3] != sc) continue;
        uint32_t t = ptr_pointee(e, v[1]);
        int l = var_location(e, ep[k]);
        if (l >= 0) { int x = l + loc_count(e, t, 0); if (x > end) end = x; continue; }
        /* bloco com locations nos membros */
        for (uint32_t i = 0; i < e->i_annot_end; i++) {
            const uint32_t *w = I(e, i);
            if (OP(w[0]) == 72 && WC(w[0]) >= 5 && w[1] == t && w[3] == D_LOCATION) {
                const uint32_t *st = defw(e, t);
                uint32_t mt = (st && OP(st[0]) == 30 && w[2] + 2 < WC(st[0])) ? st[2 + w[2]] : 0;
                int x = (int)w[4] + (mt ? loc_count(e, mt, 0) : 1);
                if (x > end) end = x;
            }
        }
    }
    return end;
}

/* saída de um built-in: variável solta (member = -1) ou membro de bloco */
typedef struct { uint32_t var, ptr_t, type; int member; } BOut;
static int find_builtin_out(const Ed *e, uint32_t builtin, BOut *o) {
    const uint32_t *ep = I(e, e->ep_i);
    for (uint32_t k = e->ep_iface; k < WC(ep[0]); k++) {
        const uint32_t *v = defw(e, ep[k]);
        if (!v || OP(v[0]) != 59 || v[3] != SC_OUTPUT) continue;
        uint32_t t = ptr_pointee(e, v[1]);
        if (var_builtin(e, ep[k]) == (int)builtin) { o->var = ep[k]; o->ptr_t = v[1]; o->type = t; o->member = -1; return 1; }
        const uint32_t *st = defw(e, t);
        if (st && OP(st[0]) == 30) {
            int m = member_builtin(e, t, builtin);
            if (m >= 0 && (uint32_t)m + 2 < WC(st[0])) { o->var = ep[k]; o->ptr_t = v[1]; o->type = st[2 + m]; o->member = m; return 1; }
        }
    }
    return 0;
}

/* algum OpStore escreve no built-in (direto ou por access chain)? */
static int builtin_written(const Ed *e, const BOut *b) {
    uint8_t *mark = calloc(e->bound, 1);
    if (!mark) return 0;
    int w_ = 0;
    if (b->member < 0) mark[b->var] = 1;
    for (uint32_t i = e->i_func; i < e->ni && !w_; i++) {
        const uint32_t *w = I(e, i); uint32_t op = OP(w[0]), wc = WC(w[0]);
        if ((op == 65 || op == 66) && wc >= 4 && w[3] < e->bound) {
            if (mark[w[3]]) mark[w[2]] = 1;
            else if (w[3] == b->var && b->member >= 0 && wc >= 5) {
                int ok; int32_t m = const_val(e, w[4], &ok);
                if (ok && m == b->member) mark[w[2]] = 1;
            }
        } else if (op == 62 && wc >= 3 && w[1] < e->bound && mark[w[1]]) w_ = 1;
    }
    free(mark);
    return w_;
}

static int array_len(const Ed *e, uint32_t t) {
    const uint32_t *w = defw(e, t);
    if (!w || OP(w[0]) != 28) return 0;
    int ok; int32_t n = const_val(e, w[3], &ok);
    return ok ? n : 0;
}

int spvemu_info(const uint32_t *w, size_t nw, const char *entry, SpvInfo *inf) {
    memset(inf, 0, sizeof *inf);
    inf->model = -1;
    Ed e;
    if (ed_init(&e, w, nw, entry)) return -1;
    inf->model = (int)e.ep_model;
    BOut b;
    if (find_builtin_out(&e, B_CLIP, &b) && builtin_written(&e, &b)) inf->clip_n = array_len(&e, b.type);
    if (find_builtin_out(&e, B_POSITION, &b)) inf->has_position = 1;
    const uint32_t *ep = I(&e, e.ep_i);
    for (uint32_t k = e.ep_iface; k < WC(ep[0]); k++) {
        const uint32_t *v = defw(&e, ep[k]);
        if (v && OP(v[0]) == 59 && v[3] == SC_INPUT && var_builtin(&e, ep[k]) == B_DRAWINDEX) inf->has_drawindex = 1;
    }
    inf->out_loc_end = loc_end(&e, SC_OUTPUT);
    inf->in_loc_end = loc_end(&e, SC_INPUT);
    ed_free(&e);
    return 0;
}

/* pontos onde um vértice termina: OpReturn da função do entry point (VS/TES) ou OpEmit*Vertex (GS) */
static int is_vertex_end(const Ed *e, uint32_t i) {
    uint32_t op = OP(I(e, i)[0]);
    if (e->ep_model == 3) return op == 218 || op == 220;
    return op == 253 && i > e->fn_begin && i < e->fn_end;
}

/* ------------------------------------------------------------------ ClipDistance -> varyings */
static int do_clip(Ed *e, int loc, int *nvec_out) {
    BOut b;
    if (!find_builtin_out(e, B_CLIP, &b) || !builtin_written(e, &b)) return 0;
    int n = array_len(e, b.type);
    const uint32_t *at = defw(e, b.type);
    if (n <= 0 || n > 8 || !at) return -1;
    uint32_t ft = at[2];
    const uint32_t *fw = defw(e, ft);
    if (!fw || OP(fw[0]) != 22 || fw[2] != 32) return -1;
    if (ft != t_float(e)) return -1;
    int nvec = (n + 3) / 4;

    /* variável Private iniciada em 1.0 (distâncias não escritas não recortam) */
    uint32_t one = c_float(e, 1.0f);
    uint32_t init = ed_id(e);
    { Buf *p = &e->pre[e->i_func]; bput(p, ((uint32_t)(3 + n) << 16) | 44); bput(p, b.type); bput(p, init); for (int i = 0; i < n; i++) bput(p, one); }
    uint32_t ptr_arr = t_ptr(e, SC_PRIVATE, b.type);
    uint32_t priv = ed_id(e);
    EMIT(&e->pre[e->i_func], 59, ptr_arr, priv, SC_PRIVATE, init);

    /* redireciona os acessos ao built-in para a variável Private */
    uint8_t *red = calloc(e->bound, 1);         /* ponteiros que agora apontam para Private */
    if (!red) return -1;
    int bad = 0;
    for (uint32_t i = e->i_func; i < e->ni && !bad; i++) {
        const uint32_t *w = I(e, i); uint32_t op = OP(w[0]), wc = WC(w[0]);
        if ((op == 65 || op == 66) && wc >= 4) {
            uint32_t base = w[3];
            int from_member = b.member >= 0 && base == b.var && wc >= 5;
            int from_var = b.member < 0 && base == b.var;
            int from_red = base < e->bound && red[base];
            if (from_member) {
                int ok; int32_t m = const_val(e, w[4], &ok);
                if (!ok) { bad = 1; break; }
                if (m != b.member) continue;
            }
            if (!from_member && !from_var && !from_red) continue;
            uint32_t pt = ptr_pointee(e, w[1]);
            if (!pt) { bad = 1; break; }
            Buf r = { 0 };
            uint32_t nt = t_ptr(e, SC_PRIVATE, pt);
            if (from_member) {
                if (wc == 5) { red[w[2]] = 1; EMIT(&r, op, ptr_arr, w[2], priv); }      /* cadeia só até o membro */
                else { bput(&r, ((wc - 1) << 16) | op); bput(&r, nt); bput(&r, w[2]); bput(&r, priv); for (uint32_t k = 5; k < wc; k++) bput(&r, w[k]); red[w[2]] = 1; }
            } else {
                bput(&r, (wc << 16) | op); bput(&r, nt); bput(&r, w[2]); bput(&r, from_var ? priv : base);
                for (uint32_t k = 4; k < wc; k++) bput(&r, w[k]);
                red[w[2]] = 1;
            }
            ed_set_rep(e, i, r.w, (uint32_t)r.n); free(r.w);
            continue;
        }
        if (op == 61 && wc >= 4 && wc <= 16 && b.member < 0 && w[3] == b.var) { uint32_t r[16]; memcpy(r, w, wc * 4); r[3] = priv; ed_set_rep(e, i, r, wc); continue; }
        if (op == 62 && wc >= 3 && wc <= 16 && b.member < 0 && w[1] == b.var) { uint32_t r[16]; memcpy(r, w, wc * 4); r[1] = priv; ed_set_rep(e, i, r, wc); continue; }
        if (op == 61 && wc >= 4 && w[3] < e->bound && red[w[3]]) continue;
        if (op == 62 && wc >= 3 && w[1] < e->bound && red[w[1]]) { if (w[2] < e->bound && red[w[2]]) bad = 1; continue; }
        if (op == 8 || op == 317) continue;
        /* qualquer outro uso de um ponteiro redirecionado (ou da variável solta) não é suportado */
        for (uint32_t k = 1; k < wc; k++) {
            if (w[k] < e->bound && (red[w[k]] || (b.member < 0 && w[k] == b.var))) { bad = 1; break; }
        }
        if (b.member >= 0 && (op == 61 || op == 62 || op == 63) && wc >= 3 && (w[op == 61 ? 3 : 1] == b.var)) bad = 1;   /* cópia do bloco inteiro */
    }
    free(red);
    if (bad) return -1;

    /* varyings de saída e cópia no fim de cada vértice */
    uint32_t v4 = t_vec4(e), pov = t_ptr(e, SC_OUTPUT, v4), pf = t_ptr(e, SC_PRIVATE, ft);
    uint32_t outs[2];
    for (int j = 0; j < nvec; j++) {
        outs[j] = ed_id(e);
        EMIT(&e->pre[e->i_func], 59, pov, outs[j], SC_OUTPUT);
        EMIT(&e->pre[e->i_annot_end], 71, outs[j], D_LOCATION, (uint32_t)(loc + j));
    }
    ed_iface_add(e, outs, (uint32_t)nvec);
    if (e->version >= 0x00010400) ed_iface_add(e, &priv, 1);
    uint32_t idx[8];
    for (int i = 0; i < n; i++) idx[i] = c_int(e, i);
    int nends = 0;
    for (uint32_t i = e->i_func; i < e->ni; i++) {
        if (!is_vertex_end(e, i)) continue;
        nends++;
        Buf *p = &e->pre[i];
        for (int j = 0; j < nvec; j++) {
            uint32_t comp[4];
            for (int c = 0; c < 4; c++) {
                int k = 4 * j + c;
                if (k >= n) { comp[c] = one; continue; }
                uint32_t pc = ed_id(e), v = ed_id(e);
                EMIT(p, 65, pf, pc, priv, idx[k]);
                EMIT(p, 61, ft, v, pc);
                comp[c] = v;
            }
            uint32_t vec = ed_id(e);
            EMIT(p, 80, v4, vec, comp[0], comp[1], comp[2], comp[3]);
            EMIT(p, 62, outs[j], vec);
        }
    }
    if (!nends) return -1;
    *nvec_out = nvec;
    return 1;
}

/* ------------------------------------------------------------------ depthClamp: z em [0, w] */
static int do_clamp(Ed *e) {
    BOut b;
    if (!find_builtin_out(e, B_POSITION, &b)) return 0;
    uint32_t v4 = t_vec4(e), ft = t_float(e);
    if (b.type != v4) return -1;
    /* comparação + OpSelect em vez de FMax/FMin com 0: min/max com limite 0 é o padrão da
     * errata Arm 3779215 do compilador Mali */
    uint32_t bt = t_bool(e), zero = c_float(e, 0.0f);
    uint32_t pov = t_ptr(e, SC_OUTPUT, v4);
    uint32_t mi = b.member >= 0 ? c_int(e, b.member) : 0;
    int nends = 0;
    for (uint32_t i = e->i_func; i < e->ni; i++) {
        if (!is_vertex_end(e, i)) continue;
        nends++;
        Buf *p = &e->pre[i];
        uint32_t ptr = b.var;
        if (b.member >= 0) { ptr = ed_id(e); EMIT(p, 65, pov, ptr, b.var, mi); }
        uint32_t v = ed_id(e), z = ed_id(e), w = ed_id(e), lo = ed_id(e), z1 = ed_id(e), hi = ed_id(e), z2 = ed_id(e), v2 = ed_id(e);
        EMIT(p, 61, v4, v, ptr);
        EMIT(p, 81, ft, z, v, 2);
        EMIT(p, 81, ft, w, v, 3);
        EMIT(p, 184, bt, lo, z, zero);          /* z < 0 */
        EMIT(p, 169, ft, z1, lo, zero, z);
        EMIT(p, 186, bt, hi, z1, w);            /* z > w */
        EMIT(p, 169, ft, z2, hi, w, z1);
        EMIT(p, 82, v4, v2, z2, v, 2);
        EMIT(p, 62, ptr, v2);
    }
    return nends ? 1 : -1;
}

/* ------------------------------------------------------------------ depthClamp exato (v4.8)
 * Último estágio: (z, w) de clip vão numa varying vec2 (interpolação perspective-correct dá o z/w
 * real em cada pixel) e gl_Position.z vira 0.5*w, que fica sempre entre 0 e w: nada é cortado no
 * near/far, e o recorte que sobra é o de w > 0 (plano do olho), como no depthClamp de verdade.
 * O fragment shader (spvemu_fs_clamp) grava gl_FragDepth = clamp(z/w) por pixel. */
static uint32_t t_vec2(Ed *e) { uint32_t a[] = { t_float(e), 2 }; return ed_find(e, 23, 1, a, 2); }

static int do_clamp_exact(Ed *e, int loc) {
    BOut b;
    if (!find_builtin_out(e, B_POSITION, &b)) return 0;
    uint32_t v4 = t_vec4(e), ft = t_float(e), v2 = t_vec2(e);
    if (b.type != v4) return -1;
    uint32_t pov = t_ptr(e, SC_OUTPUT, v4), pv2 = t_ptr(e, SC_OUTPUT, v2), half = c_float(e, 0.5f);
    uint32_t mi = b.member >= 0 ? c_int(e, b.member) : 0;
    uint32_t out = ed_id(e);
    EMIT(&e->pre[e->i_func], 59, pv2, out, SC_OUTPUT);
    EMIT(&e->pre[e->i_annot_end], 71, out, D_LOCATION, (uint32_t)loc);
    ed_iface_add(e, &out, 1);
    int nends = 0;
    for (uint32_t i = e->i_func; i < e->ni; i++) {
        if (!is_vertex_end(e, i)) continue;
        nends++;
        Buf *p = &e->pre[i];
        uint32_t ptr = b.var;
        if (b.member >= 0) { ptr = ed_id(e); EMIT(p, 65, pov, ptr, b.var, mi); }
        uint32_t v = ed_id(e), z = ed_id(e), w = ed_id(e), zw = ed_id(e), hz = ed_id(e), v2b = ed_id(e);
        EMIT(p, 61, v4, v, ptr);
        EMIT(p, 81, ft, z, v, 2);
        EMIT(p, 81, ft, w, v, 3);
        EMIT(p, 80, v2, zw, z, w);
        EMIT(p, 62, out, zw);
        EMIT(p, 133, ft, hz, w, half);
        EMIT(p, 82, v4, v2b, hz, v, 2);
        EMIT(p, 62, ptr, v2b);
    }
    return nends ? 1 : -1;
}

/* ------------------------------------------------------------------ depth_clip_control (v4.9.4)
 * negativeOneToOne: o app escreve z em [-w, w] (convenção do OpenGL); vira (z + w) / 2. */
static int do_negz(Ed *e) {
    BOut b;
    if (!find_builtin_out(e, B_POSITION, &b)) return 0;
    uint32_t v4 = t_vec4(e), ft = t_float(e);
    if (b.type != v4) return -1;
    uint32_t pov = t_ptr(e, SC_OUTPUT, v4), half = c_float(e, 0.5f);
    uint32_t mi = b.member >= 0 ? c_int(e, b.member) : 0;
    int nends = 0;
    for (uint32_t i = e->i_func; i < e->ni; i++) {
        if (!is_vertex_end(e, i)) continue;
        nends++;
        Buf *p = &e->pre[i];
        uint32_t ptr = b.var;
        if (b.member >= 0) { ptr = ed_id(e); EMIT(p, 65, pov, ptr, b.var, mi); }
        uint32_t v = ed_id(e), z = ed_id(e), w = ed_id(e), s_ = ed_id(e), h = ed_id(e), v2 = ed_id(e);
        EMIT(p, 61, v4, v, ptr);
        EMIT(p, 81, ft, z, v, 2);
        EMIT(p, 81, ft, w, v, 3);
        EMIT(p, 129, ft, s_, z, w);
        EMIT(p, 133, ft, h, s_, half);
        EMIT(p, 82, v4, v2, h, v, 2);
        EMIT(p, 62, ptr, v2);
    }
    return nends ? 1 : -1;
}

/* ------------------------------------------------------------------ push constants do vkshim
 * gl_DrawID (v4.7) e draw_indirect_count (v4.9.4) num bloco de push constant só:
 *   off : int com o índice do draw (o vkshim grava antes de cada draw)
 *   off8: uvec2 com o endereço (buffer device address) do contador; 0 = draw comum.
 * Com off8, no fim do vertex shader: se endereço != 0 e índice >= *endereço, o vértice vai para
 * fora da tela (o draw some). */
static uint32_t t_uint(Ed *e) { uint32_t a[] = { 32, 0 }; return ed_find(e, 21, 1, a, 2); }
static uint32_t c_uint(Ed *e, uint32_t v) { uint32_t a[] = { t_uint(e), v }; return ed_find(e, 43, 2, a, 2); }

static int do_pc(Ed *e, int off, int off8) {
    uint32_t dvar = 0, it = 0;
    const uint32_t *ep = I(e, e->ep_i);
    for (uint32_t k = e->ep_iface; k < WC(ep[0]); k++) {
        const uint32_t *v = defw(e, ep[k]);
        if (v && OP(v[0]) == 59 && v[3] == SC_INPUT && var_builtin(e, ep[k]) == B_DRAWINDEX) { dvar = ep[k]; it = ptr_pointee(e, v[1]); }
    }
    int need_cnt = off8 >= 0 && e->ep_model == 0;
    if (!dvar && !need_cnt) return 0;
    /* bloco de push constant existente: os tipos dos membros novos são declarados antes dele */
    uint32_t pv0 = 0, st0 = 0;
    for (uint32_t i = 0; i < e->i_func; i++) { const uint32_t *w = I(e, i); if (OP(w[0]) == 59 && WC(w[0]) >= 4 && w[3] == SC_PUSH) { pv0 = w[2]; st0 = ptr_pointee(e, w[1]); } }
    uint32_t tat = (pv0 && st0 < e->bound0 && e->def[st0]) ? e->def[st0] : e->i_func;
    if (dvar) { const uint32_t *itw = defw(e, it); if (!itw || OP(itw[0]) != 21 || itw[2] != 32) return -1; }
    else { uint32_t a[] = { 32, 1 }; it = ed_find_at(e, 21, 1, a, 2, tat); }
    BOut pos;
    if (need_cnt && (!find_builtin_out(e, B_POSITION, &pos) || pos.type != t_vec4(e))) need_cnt = 0;
    if (!dvar && !need_cnt) return 0;
    uint32_t tu, tuv2 = 0;
    { uint32_t a[] = { 32, 0 }; tu = ed_find_at(e, 21, 1, a, 2, tat); }
    if (need_cnt) { uint32_t a[] = { tu, 2 }; tuv2 = ed_find_at(e, 23, 1, a, 2, tat); }
    /* bloco de push constant existente (no máximo um por entry point) */
    uint32_t pvar = 0, st = 0;
    for (uint32_t i = 0; i < e->i_func; i++) {
        const uint32_t *w = I(e, i);
        if (OP(w[0]) == 59 && WC(w[0]) >= 4 && w[3] == SC_PUSH) {
            if (pvar) return -1;
            pvar = w[2]; st = ptr_pointee(e, w[1]);
        }
    }
    uint32_t mdid, maddr = 0;
    if (pvar) {
        const uint32_t *sw = defw(e, st);
        if (!sw || OP(sw[0]) != 30) return -1;
        uint32_t wc = WC(sw[0]);
        uint32_t r[64];
        if (wc + 2 > 64) return -1;
        memcpy(r, sw, wc * 4);
        mdid = wc - 2; r[wc] = it;
        uint32_t nwc = wc + 1;
        if (need_cnt) { maddr = wc - 1; r[nwc++] = tuv2; }
        r[0] = (nwc << 16) | 30;
        ed_set_rep(e, e->def[st], r, nwc);
        EMIT(&e->pre[e->i_annot_end], 72, st, mdid, D_OFFSET, (uint32_t)off);
        if (need_cnt) EMIT(&e->pre[e->i_annot_end], 72, st, maddr, D_OFFSET, (uint32_t)off8);
    } else {
        st = ed_id(e); mdid = 0;
        if (need_cnt) { maddr = 1; EMIT(&e->pre[e->i_func], 30, st, it, tuv2); }
        else EMIT(&e->pre[e->i_func], 30, st, it);
        EMIT(&e->pre[e->i_annot_end], 71, st, D_BLOCK);
        EMIT(&e->pre[e->i_annot_end], 72, st, 0, D_OFFSET, (uint32_t)off);
        if (need_cnt) EMIT(&e->pre[e->i_annot_end], 72, st, 1, D_OFFSET, (uint32_t)off8);
        uint32_t pt = ed_id(e);   /* ponteiro novo para o struct novo */
        EMIT(&e->pre[e->i_func], 32, pt, SC_PUSH, st);
        pvar = ed_id(e);
        EMIT(&e->pre[e->i_func], 59, pt, pvar, SC_PUSH);
        if (e->version >= 0x00010400) ed_iface_add(e, &pvar, 1);
    }
    uint32_t ppi = t_ptr(e, SC_PUSH, it), mc = c_int(e, (int32_t)mdid);
    int n = 0;
    if (dvar)
        for (uint32_t i = e->i_func; i < e->ni; i++) {
            const uint32_t *w = I(e, i); uint32_t op = OP(w[0]), wc = WC(w[0]);
            if (op == 61 && wc >= 4 && w[3] == dvar) {
                uint32_t ac = ed_id(e);
                Buf r = { 0 };
                EMIT(&r, 65, ppi, ac, pvar, mc);
                EMIT(&r, 61, w[1], w[2], ac);
                ed_set_rep(e, i, r.w, (uint32_t)r.n); free(r.w);
                n++;
                continue;
            }
            if (op == 8 || op == 317) continue;
            for (uint32_t k = 1; k < wc; k++) if (w[k] == dvar) return -1;
        }
    if (need_cnt) {
        uint32_t bt = t_bool(e), v4 = t_vec4(e), ft = t_float(e);
        uint32_t tul = 0; { uint32_t a[] = { 64, 0 }; tul = ed_find(e, 21, 1, a, 2); }
        uint32_t ppsb = t_ptr(e, 5349, tu), ppa = t_ptr(e, SC_PUSH, tuv2), pov = t_ptr(e, SC_OUTPUT, v4);
        uint32_t cma = c_int(e, (int32_t)maddr), cu0 = c_uint(e, 0), c2 = c_float(e, 2.0f), c1 = c_float(e, 1.0f);
        uint32_t fora; { uint32_t a[] = { v4, c2, c2, c2, c1 }; fora = ed_find(e, 44, 2, a, 5); }
        uint32_t pmi = pos.member >= 0 ? c_int(e, pos.member) : 0;
        int nends = 0;
        for (uint32_t i = e->i_func; i < e->ni; i++) {
            if (!is_vertex_end(e, i)) continue;
            nends++;
            Buf *p = &e->pre[i];
            uint32_t pa = ed_id(e), a = ed_id(e), ax = ed_id(e), ay = ed_id(e), or_ = ed_id(e), nz = ed_id(e);
            uint32_t Lt = ed_id(e), Lm = ed_id(e), Lk = ed_id(e), Lm2 = ed_id(e);
            EMIT(p, 65, ppa, pa, pvar, cma);
            EMIT(p, 61, tuv2, a, pa);
            EMIT(p, 81, tu, ax, a, 0);
            EMIT(p, 81, tu, ay, a, 1);
            EMIT(p, 197, tu, or_, ax, ay);                 /* OpBitwiseOr */
            EMIT(p, 171, bt, nz, or_, cu0);                /* OpINotEqual */
            EMIT(p, 247, Lm, 0);
            EMIT(p, 250, nz, Lt, Lm);
            EMIT(p, 248, Lt);
            uint32_t u64 = ed_id(e), ptr = ed_id(e), cnt = ed_id(e), pd = ed_id(e), d = ed_id(e), du = ed_id(e), dead = ed_id(e);
            EMIT(p, 124, tul, u64, a);                     /* OpBitcast uvec2 -> u64 */
            EMIT(p, 120, ppsb, ptr, u64);                  /* OpConvertUToPtr */
            EMIT(p, 61, tu, cnt, ptr, 2, 4);               /* Load Aligned 4 */
            EMIT(p, 65, ppi, pd, pvar, mc);
            EMIT(p, 61, it, d, pd);
            EMIT(p, 124, tu, du, d);
            EMIT(p, 174, bt, dead, du, cnt);               /* OpUGreaterThanEqual */
            EMIT(p, 247, Lm2, 0);
            EMIT(p, 250, dead, Lk, Lm2);
            EMIT(p, 248, Lk);
            uint32_t pp = pos.var;
            if (pos.member >= 0) { pp = ed_id(e); EMIT(p, 65, pov, pp, pos.var, pmi); }
            EMIT(p, 62, pp, fora);
            EMIT(p, 249, Lm2);
            EMIT(p, 248, Lm2);
            EMIT(p, 249, Lm);
            EMIT(p, 248, Lm);
        }
        (void)ft;
        if (!nends) return -1;
        /* Int64 + PhysicalStorageBufferAddresses, modelo de endereçamento e extensão */
        int h11 = 0, h5347 = 0, hext = 0; uint32_t first_non_cap = 0, mm = 0;
        for (uint32_t i = 0; i < e->i_func; i++) {
            const uint32_t *x = I(e, i);
            if (OP(x[0]) == 17) { if (x[1] == 11) h11 = 1; if (x[1] == 5347) h5347 = 1; }
            else if (!first_non_cap) first_non_cap = i;
            if (OP(x[0]) == 10 && strstr((const char *)&x[1], "physical_storage_buffer")) hext = 1;
            if (OP(x[0]) == 14) mm = i;
        }
        if (!h11) EMIT(&e->pre[0], 17, 11);
        if (!h5347) EMIT(&e->pre[0], 17, 5347);
        if (!hext && e->version < 0x00010500 && first_non_cap) {
            static const char nm[] = "SPV_KHR_physical_storage_buffer";
            uint32_t wd[9] = { 0 }; memcpy(wd, nm, sizeof nm);
            binst(&e->pre[first_non_cap], 10, wd, (uint32_t)((sizeof nm + 3) / 4));
        }
        if (mm) { const uint32_t *x = I(e, mm); if (x[1] == 0) { uint32_t r[3] = { (3u << 16) | 14, 5348, x[2] }; ed_set_rep(e, mm, r, 3); } else if (x[1] != 5348) return -1; }
        n++;
    }
    return n ? 1 : 0;
}

uint32_t *spvemu_prerast2(const uint32_t *w, size_t nw, const char *entry, int clip_loc, int clamp,
                          int drawid_off, int count_off8, int negz, size_t *out_nw, int *clip_nvec, unsigned *done) {
    *done = 0; *clip_nvec = 0;
    Ed e;
    oom = 0;
    if (ed_init(&e, w, nw, entry)) return NULL;
    if (e.ep_model > 3 || e.ep_model == 1) { ed_free(&e); return NULL; }
    if (clip_loc >= 0) { int r = do_clip(&e, clip_loc, clip_nvec); if (r < 0) goto fail; if (r) *done |= SPVEMU_DID_CLIP; }
    if (negz) { int r = do_negz(&e); if (r < 0) goto fail; if (r) *done |= SPVEMU_DID_NEGZ; }
    if (clamp >= SPVEMU_CLAMP_EXACT) {
        int r = do_clamp_exact(&e, clamp - SPVEMU_CLAMP_EXACT); if (r < 0) goto fail; if (r) *done |= SPVEMU_DID_CLAMP | SPVEMU_DID_CLAMPX;
    } else if (clamp) { int r = do_clamp(&e); if (r < 0) goto fail; if (r) *done |= SPVEMU_DID_CLAMP; }
    if (drawid_off >= 0 && e.ep_model == 0) {
        int r = do_pc(&e, drawid_off, count_off8); if (r < 0) goto fail;
        if (r) *done |= SPVEMU_DID_DRAWID | (count_off8 >= 0 ? SPVEMU_DID_COUNT : 0);
    }
    if (!*done) goto fail;
    uint32_t *out = ed_build(&e, out_nw);
    ed_free(&e);
    if (!out) *done = 0;
    return out;
fail:
    ed_free(&e);
    *done = 0;
    return NULL;
}
uint32_t *spvemu_prerast(const uint32_t *w, size_t nw, const char *entry, int clip_loc, int clamp,
                         int drawid_off, size_t *out_nw, int *clip_nvec, unsigned *done) {
    return spvemu_prerast2(w, nw, entry, clip_loc, clamp, drawid_off, -1, 0, out_nw, clip_nvec, done);
}

/* ------------------------------------------------------------------ fragment shader */
static void emit_kill_body(Ed *e, Buf *p, const uint32_t *ins, int nvec, uint32_t lk, uint32_t lm) {
    uint32_t ft = t_float(e), v4 = t_vec4(e), bt = t_bool(e), zero = c_float(e, 0.0f);
    uint32_t acc = 0;
    for (int j = 0; j < nvec; j++) {
        uint32_t a = ed_id(e);
        EMIT(p, 61, v4, a, ins[j]);
        for (uint32_t c = 0; c < 4; c++) {
            uint32_t x = ed_id(e), lt = ed_id(e);
            EMIT(p, 81, ft, x, a, c);
            EMIT(p, 184, bt, lt, x, zero);
            if (!acc) acc = lt;
            else { uint32_t o = ed_id(e); EMIT(p, 166, bt, o, acc, lt); acc = o; }
        }
    }
    EMIT(p, 247, lm, 0);
    EMIT(p, 250, acc, lk, lm);
    EMIT(p, 248, lk);
    EMIT(p, 252);
    EMIT(p, 248, lm);
}

uint32_t *spvemu_fs_clip(const uint32_t *w, size_t nw, const char *entry, int loc, int nvec, size_t *out_nw) {
    Ed e;
    oom = 0;
    if (nvec < 1 || nvec > 2 || ed_init(&e, w, nw, entry)) return NULL;
    if (e.ep_model != 4) { ed_free(&e); return NULL; }
    const uint32_t *fn = I(&e, e.fn_begin);
    uint32_t void_t = fn[1], fn_t = fn[4];
    uint32_t v4 = t_vec4(&e), piv = t_ptr(&e, SC_INPUT, v4);
    uint32_t ins[2];
    for (int j = 0; j < nvec; j++) {
        ins[j] = ed_id(&e);
        EMIT(&e.pre[e.i_func], 59, piv, ins[j], SC_INPUT);
        EMIT(&e.pre[e.i_annot_end], 71, ins[j], D_LOCATION, (uint32_t)(loc + j));
    }
    ed_iface_add(&e, ins, (uint32_t)nvec);
    /* função nova no fim: void vkshim_clip() { if (alguma distância < 0) discard; } */
    uint32_t f = ed_id(&e), l0 = ed_id(&e), lk = ed_id(&e), lm = ed_id(&e);
    Buf *p = &e.pre[e.ni];
    EMIT(p, 54, void_t, f, 0, fn_t);
    EMIT(p, 248, l0);
    emit_kill_body(&e, p, ins, nvec, lk, lm);
    EMIT(p, 253);
    EMIT(p, 56);
    /* chamada no começo do main, depois das OpVariable do 1º bloco */
    uint32_t at = e.fn_begin + 1;
    while (at < e.fn_end && OP(I(&e, at)[0]) != 248) at++;
    at++;
    while (at < e.fn_end) { uint32_t op = OP(I(&e, at)[0]); if (op != 59 && op != 8 && op != 317) break; at++; }
    if (at >= e.fn_end) { ed_free(&e); return NULL; }
    uint32_t call = ed_id(&e);
    EMIT(&e.pre[at], 57, void_t, call, f);
    uint32_t *out = ed_build(&e, out_nw);
    ed_free(&e);
    return out;
}

uint32_t *spvemu_fs_new(int loc, int nvec, size_t *out_nw) {
    if (nvec < 1 || nvec > 2) return NULL;
    /* ids fixos */
    enum { VOID = 1, FNT, FLT, V4, PIV, BOOL, ZERO, MAIN, L0, LK, LM, IN0, IN1, A0, NEXT };
    Buf b = { 0 };
    oom = 0;
    bput(&b, 0x07230203); bput(&b, 0x00010000); bput(&b, 0); bput(&b, 0); bput(&b, 0);
    EMIT(&b, 17, 1);                                    /* Shader */
    EMIT(&b, 14, 0, 1);                                 /* Logical GLSL450 */
    { uint32_t a[8] = { 4, MAIN, 0x6e69616d, 0, IN0, IN1 }; binst(&b, 15, a, nvec == 2 ? 6 : 5); }
    EMIT(&b, 16, MAIN, 7);                              /* OriginUpperLeft */
    EMIT(&b, 71, IN0, D_LOCATION, (uint32_t)loc);
    if (nvec == 2) EMIT(&b, 71, IN1, D_LOCATION, (uint32_t)loc + 1);
    EMIT(&b, 19, VOID);
    EMIT(&b, 33, FNT, VOID);
    EMIT(&b, 22, FLT, 32);
    EMIT(&b, 23, V4, FLT, 4);
    EMIT(&b, 32, PIV, SC_INPUT, V4);
    EMIT(&b, 20, BOOL);
    EMIT(&b, 43, FLT, ZERO, 0);
    EMIT(&b, 59, PIV, IN0, SC_INPUT);
    if (nvec == 2) EMIT(&b, 59, PIV, IN1, SC_INPUT);
    EMIT(&b, 54, VOID, MAIN, 0, FNT);
    EMIT(&b, 248, L0);
    uint32_t id = A0, acc = 0;
    for (int j = 0; j < nvec; j++) {
        uint32_t a = id++;
        EMIT(&b, 61, V4, a, j ? IN1 : IN0);
        for (uint32_t c = 0; c < 4; c++) {
            uint32_t x = id++, lt = id++;
            EMIT(&b, 81, FLT, x, a, c);
            EMIT(&b, 184, BOOL, lt, x, ZERO);
            if (!acc) acc = lt; else { uint32_t o = id++; EMIT(&b, 166, BOOL, o, acc, lt); acc = o; }
        }
    }
    EMIT(&b, 247, LM, 0);
    EMIT(&b, 250, acc, LK, LM);
    EMIT(&b, 248, LK);
    EMIT(&b, 252);
    EMIT(&b, 248, LM);
    EMIT(&b, 253);
    EMIT(&b, 56);
    if (oom) { free(b.w); return NULL; }
    b.w[3] = id;
    *out_nw = b.n;
    return b.w;
}

/* ------------------------------------------------------------------ depthClamp exato: fragment shader
 * Lê a varying (z, w) em loc e calcula d = clamp(minD + z/w*(maxD-minD)). Se o shader não grava
 * gl_FragDepth, passa a gravar d (DepthReplacing). Se grava, cada escrita é grampeada no intervalo.
 * Leituras de gl_FragCoord passam a ver z = d (o z rasterizado virou a constante 0.5). */
static int find_builtin_var(const Ed *e, uint32_t sc, uint32_t builtin, uint32_t *ptr_t) {
    const uint32_t *ep = I(e, e->ep_i);
    for (uint32_t k = e->ep_iface; k < WC(ep[0]); k++) {
        const uint32_t *v = defw(e, ep[k]);
        if (!v || OP(v[0]) != 59 || v[3] != sc) continue;
        if (var_builtin(e, ep[k]) == (int)builtin) { if (ptr_t) *ptr_t = v[1]; return (int)ep[k]; }
        const uint32_t *st = defw(e, ptr_pointee(e, v[1]));
        if (st && OP(st[0]) == 30 && member_builtin(e, ptr_pointee(e, v[1]), builtin) >= 0) return -1;   /* em bloco: não suportado */
    }
    return 0;
}

/* clamp por comparação + OpSelect (min/max com 0 cai na errata Arm 3779215) */
static uint32_t emit_clamp(Ed *e, Buf *p, uint32_t x, uint32_t lo, uint32_t hi) {
    uint32_t ft = t_float(e), bt = t_bool(e);
    uint32_t c1 = ed_id(e), s1 = ed_id(e), c2 = ed_id(e), s2 = ed_id(e);
    EMIT(p, 184, bt, c1, x, lo);
    EMIT(p, 169, ft, s1, c1, lo, x);
    EMIT(p, 186, bt, c2, s1, hi);
    EMIT(p, 169, ft, s2, c2, hi, s1);
    return s2;
}

uint32_t *spvemu_fs_clamp(const uint32_t *w, size_t nw, const char *entry, int loc, float mind, float maxd, size_t *out_nw) {
    Ed e;
    oom = 0;
    if (loc < 0 || ed_init(&e, w, nw, entry)) return NULL;
    if (e.ep_model != 4) goto fail;
    /* EarlyFragmentTests ignora gl_FragDepth; DepthGreater/Less/Unchanged prometem algo que deixa de valer */
    uint32_t last_ep = e.ep_i;
    for (uint32_t i = 0; i < e.i_annot_end; i++) {
        const uint32_t *x = I(&e, i);
        if (OP(x[0]) == 15) last_ep = i;
        if (OP(x[0]) == 16 && WC(x[0]) >= 3 && x[1] == e.ep_fn && (x[2] == 9 || x[2] == 14 || x[2] == 15 || x[2] == 16)) goto fail;
    }
    uint32_t fd_pt = 0, fc_pt = 0;
    int fdv = find_builtin_var(&e, SC_OUTPUT, 22, &fd_pt);
    int fcv = find_builtin_var(&e, SC_INPUT, 15, &fc_pt);
    if (fdv < 0 || fcv < 0) goto fail;
    uint32_t ft = t_float(&e), v2 = t_vec2(&e), v4 = t_vec4(&e);
    if (fcv && ptr_pointee(&e, fc_pt) != v4) goto fail;
    if (fdv && ptr_pointee(&e, fd_pt) != ft) goto fail;
    float lo_f = mind < maxd ? mind : maxd, hi_f = mind < maxd ? maxd : mind;
    uint32_t lo = c_float(&e, lo_f), hi = c_float(&e, hi_f);
    uint32_t pf_priv = t_ptr(&e, SC_PRIVATE, ft);

    /* entrada (z, w) */
    uint32_t in = ed_id(&e);
    EMIT(&e.pre[e.i_func], 59, t_ptr(&e, SC_INPUT, v2), in, SC_INPUT);
    EMIT(&e.pre[e.i_annot_end], 71, in, D_LOCATION, (uint32_t)loc);
    ed_iface_add(&e, &in, 1);
    /* profundidade grampeada guardada numa Private (as escritas podem estar em outras funções) */
    uint32_t dzp = ed_id(&e);
    EMIT(&e.pre[e.i_func], 59, pf_priv, dzp, SC_PRIVATE);
    if (e.version >= 0x00010400) ed_iface_add(&e, &dzp, 1);
    uint32_t fcp = 0;
    if (fcv) {
        fcp = ed_id(&e);
        EMIT(&e.pre[e.i_func], 59, t_ptr(&e, SC_PRIVATE, v4), fcp, SC_PRIVATE);
        if (e.version >= 0x00010400) ed_iface_add(&e, &fcp, 1);
    }
    uint32_t fdo = (uint32_t)fdv;
    if (!fdv) {
        fdo = ed_id(&e);
        EMIT(&e.pre[e.i_func], 59, t_ptr(&e, SC_OUTPUT, ft), fdo, SC_OUTPUT);
        EMIT(&e.pre[e.i_annot_end], 71, fdo, D_BUILTIN, 22);
        EMIT(&e.pre[last_ep + 1], 16, e.ep_fn, 12);              /* DepthReplacing */
        ed_iface_add(&e, &fdo, 1);
    }

    /* usos do gl_FragCoord e escritas do gl_FragDepth no código original */
    for (uint32_t i = e.i_func; i < e.ni; i++) {
        const uint32_t *x = I(&e, i); uint32_t op = OP(x[0]), wc = WC(x[0]);
        if (op == 8 || op == 317) continue;
        if (fcv) {
            if (op == 61 && wc >= 4 && x[3] == (uint32_t)fcv) {
                uint32_t r[16]; if (wc > 16) goto fail;
                memcpy(r, x, wc * 4); r[3] = fcp; ed_set_rep(&e, i, r, wc); continue;
            }
            if ((op == 65 || op == 66) && wc >= 4 && x[3] == (uint32_t)fcv) {
                uint32_t pt = ptr_pointee(&e, x[1]);
                uint32_t r[16]; if (!pt || wc > 16) goto fail;
                memcpy(r, x, wc * 4); r[1] = t_ptr(&e, SC_PRIVATE, pt); r[3] = fcp; ed_set_rep(&e, i, r, wc); continue;
            }
        }
        if (fdv && op == 62 && wc >= 3 && x[1] == (uint32_t)fdv) {
            Buf r = { 0 };
            uint32_t c = emit_clamp(&e, &r, x[2], lo, hi);
            EMIT(&r, 62, (uint32_t)fdv, c);
            ed_set_rep(&e, i, r.w, (uint32_t)r.n); free(r.w);
            continue;
        }
        if (fdv && op == 61 && wc >= 4 && x[3] == (uint32_t)fdv) continue;
        for (uint32_t k = 1; k < wc; k++)
            if ((fcv && x[k] == (uint32_t)fcv) || (fdv && x[k] == (uint32_t)fdv)) goto fail;
    }

    /* começo do main, depois das OpVariable do 1º bloco */
    uint32_t at = e.fn_begin + 1;
    while (at < e.fn_end && OP(I(&e, at)[0]) != 248) at++;
    at++;
    while (at < e.fn_end) { uint32_t op = OP(I(&e, at)[0]); if (op != 59 && op != 8 && op != 317) break; at++; }
    if (at >= e.fn_end) goto fail;
    Buf *p = &e.pre[at];
    uint32_t a = ed_id(&e), z = ed_id(&e), ww = ed_id(&e), d = ed_id(&e);
    EMIT(p, 61, v2, a, in);
    EMIT(p, 81, ft, z, a, 0);
    EMIT(p, 81, ft, ww, a, 1);
    EMIT(p, 136, ft, d, z, ww);
    if (mind != 0.0f || maxd != 1.0f) {
        uint32_t m = ed_id(&e), s = ed_id(&e);
        EMIT(p, 133, ft, m, d, c_float(&e, maxd - mind));
        EMIT(p, 129, ft, s, m, c_float(&e, mind));
        d = s;
    }
    uint32_t dz = emit_clamp(&e, p, d, lo, hi);
    EMIT(p, 62, dzp, dz);
    if (!fdv) EMIT(p, 62, fdo, dz);
    if (fcv) {
        uint32_t f = ed_id(&e), f2 = ed_id(&e);
        EMIT(p, 61, v4, f, (uint32_t)fcv);
        EMIT(p, 82, v4, f2, dz, f, 2);
        EMIT(p, 62, fcp, f2);
    }
    uint32_t *out = ed_build(&e, out_nw);
    ed_free(&e);
    return out;
fail:
    ed_free(&e);
    return NULL;
}

/* FS mínimo só com a profundidade (pipeline sem fragment shader e sem escrita de cor) */
uint32_t *spvemu_fs_depth_new(int loc, float mind, float maxd, size_t *out_nw) {
    enum { VOID = 1, FNT, MAIN, L0, NEXT };
    Buf b = { 0 };
    oom = 0;
    bput(&b, 0x07230203); bput(&b, 0x00010000); bput(&b, 0); bput(&b, NEXT); bput(&b, 0);
    EMIT(&b, 17, 1);
    EMIT(&b, 14, 0, 1);
    EMIT(&b, 15, 4, MAIN, 0x6e69616d, 0);
    EMIT(&b, 16, MAIN, 7);
    EMIT(&b, 19, VOID);
    EMIT(&b, 33, FNT, VOID);
    EMIT(&b, 54, VOID, MAIN, 0, FNT);
    EMIT(&b, 248, L0);
    EMIT(&b, 253);
    EMIT(&b, 56);
    if (oom) { free(b.w); return NULL; }
    size_t n;
    uint32_t *r = spvemu_fs_clamp(b.w, b.n, "main", loc, mind, maxd, &n);
    free(b.w);
    if (r) *out_nw = n;
    return r;
}

/* ------------------------------------------------------------------ fillModeNonSolid por geometry shader
 * O VS (último estágio) continua igual; um GS gerado aqui recebe cada triângulo e emite as três arestas
 * (line strip v0 v1 v2 v0) ou os três vértices (points), repassando gl_Position e todas as varyings do
 * usuário. Varyings Flat saem do vértice 0 (o provocante do triângulo). Com cull mode, front face e
 * viewport estáticos o GS faz o culling do triângulo antes (pelo determinante homogêneo); sem isso todas
 * as arestas aparecem. */
typedef struct { uint32_t var, type; uint32_t deco[8][2]; int ndeco, flat; } UOut;

static uint32_t gs_copy_type(const Ed *e, Buf *t, uint32_t *map, uint32_t sid, uint32_t *next,
                             uint32_t F, uint32_t V4, uint32_t SI, uint32_t UI, int depth) {
    if (sid >= e->bound0 || depth > 6) return 0;
    if (map[sid]) return map[sid];
    const uint32_t *w = defw(e, sid);
    if (!w) return 0;
    uint32_t id = 0;
    switch (OP(w[0])) {
    case 21:
        if (w[2] == 32) id = w[3] ? SI : UI;
        else { id = (*next)++; EMIT(t, 21, id, w[2], w[3]); }
        break;
    case 22:
        if (w[2] == 32) id = F;
        else { id = (*next)++; EMIT(t, 22, id, w[2]); }
        break;
    case 23: {
        uint32_t c = gs_copy_type(e, t, map, w[2], next, F, V4, SI, UI, depth + 1);
        if (!c) return 0;
        if (c == F && w[3] == 4) id = V4;
        else { id = (*next)++; EMIT(t, 23, id, c, w[3]); }
        break;
    }
    case 24: {
        uint32_t c = gs_copy_type(e, t, map, w[2], next, F, V4, SI, UI, depth + 1);
        if (!c) return 0;
        id = (*next)++; EMIT(t, 24, id, c, w[3]);
        break;
    }
    case 28: {
        uint32_t c = gs_copy_type(e, t, map, w[2], next, F, V4, SI, UI, depth + 1);
        int ok; int32_t n = const_val(e, w[3], &ok);
        if (!c || !ok || n <= 0) return 0;
        uint32_t len = (*next)++;
        EMIT(t, 43, UI, len, (uint32_t)n);
        id = (*next)++; EMIT(t, 28, id, c, len);
        break;
    }
    default:
        return 0;
    }
    map[sid] = id;
    return id;
}

uint32_t *spvemu_gs_poly(const uint32_t *w, size_t nw, const char *entry, int points, int cull, size_t *out_nw) {
    Ed e;
    oom = 0;
    if (ed_init(&e, w, nw, entry)) return NULL;
    uint32_t *out = NULL, *map = NULL;
    Buf hd = { 0 }, dc = { 0 }, ty = { 0 }, fn = { 0 }, caps = { 0 };   /* todos antes do 1º goto */
    UOut uo[32]; int nuo = 0;
    if (e.ep_model != 0) goto done;
    /* saídas do VS */
    const uint32_t *ep = I(&e, e.ep_i);
    int have_pos = 0;
    for (uint32_t k = e.ep_iface; k < WC(ep[0]); k++) {
        const uint32_t *v = defw(&e, ep[k]);
        if (!v || OP(v[0]) != 59 || v[3] != SC_OUTPUT) continue;
        uint32_t t = ptr_pointee(&e, v[1]);
        int bi = var_builtin(&e, ep[k]);
        if (bi == B_POSITION) { have_pos = 1; continue; }
        if (bi == 1) continue;                               /* PointSize: o GS da G76 não escreve */
        if (bi >= 0) goto done;                              /* outro built-in (Layer, ViewportIndex...) */
        const uint32_t *st = defw(&e, t);
        if (st && OP(st[0]) == 30) {
            /* bloco: só gl_PerVertex com Position/PointSize */
            int ok_blk = 1, pos_blk = 0;
            for (uint32_t m = 0; m + 2 < WC(st[0]); m++) {
                int mb = -1;
                for (int bb = 0; bb <= 4; bb++) if (member_builtin(&e, t, (uint32_t)bb) == (int)m) mb = bb;
                if (mb == B_POSITION) pos_blk = 1;
                else if (mb != 1 && mb != 3 && mb != 4) ok_blk = 0;    /* PointSize e clip/cull não são repassados */
            }
            if (!ok_blk) goto done;
            if (pos_blk) have_pos = 1;
            continue;
        }
        if (var_location(&e, ep[k]) < 0 || nuo >= 32) goto done;
        UOut *u = &uo[nuo++];
        memset(u, 0, sizeof *u);
        u->var = ep[k]; u->type = t;
        for (uint32_t i = 0; i < e.i_annot_end; i++) {
            const uint32_t *d = I(&e, i);
            if (OP(d[0]) != 71 || WC(d[0]) < 3 || d[1] != ep[k]) continue;
            uint32_t dd = d[2];
            if (dd == 14) u->flat = 1;
            if ((dd == D_LOCATION || dd == 31 /* Component */) && WC(d[0]) >= 4 && u->ndeco < 8) { u->deco[u->ndeco][0] = dd; u->deco[u->ndeco][1] = d[3]; u->ndeco++; }
            else if ((dd == 14 || dd == 13 || dd == 16 || dd == 17) && u->ndeco < 8) { u->deco[u->ndeco][0] = dd; u->deco[u->ndeco][1] = ~0u; u->ndeco++; }
        }
    }
    if (!have_pos) goto done;
    map = calloc(e.bound0, 4);
    if (!map) goto done;
    enum { VOID = 1, FNT, F, V4, SI, UI, B, U3, I0, I1, I2, F0, PVI, PVO, AIN, PAIN, PVOP, GIN, GOUT, PIV4, POV4, MAIN, L0, FIRST };
    uint32_t next = FIRST;
    /* cabeçalho: capabilities e extensões do VS + Geometry */
    for (uint32_t i = 0; i < e.ep_i; i++) {
        const uint32_t *x = I(&e, i); uint32_t op = OP(x[0]);
        if (op == 17 && (x[1] == 32 || x[1] == 33 || x[1] == 1 || x[1] == 2)) continue;
        if (op == 17 || op == 10 || op == 14) binst(&hd, op, &x[1], WC(x[0]) - 1);
    }
    /* Shader e Geometry vão antes de tudo */
    EMIT(&caps, 17, 1); EMIT(&caps, 17, 2);
    /* tipos base */
    EMIT(&ty, 19, VOID); EMIT(&ty, 33, FNT, VOID);
    EMIT(&ty, 22, F, 32); EMIT(&ty, 23, V4, F, 4);
    EMIT(&ty, 21, SI, 32, 1); EMIT(&ty, 21, UI, 32, 0); EMIT(&ty, 20, B);
    EMIT(&ty, 43, UI, U3, 3); EMIT(&ty, 43, SI, I0, 0); EMIT(&ty, 43, SI, I1, 1); EMIT(&ty, 43, SI, I2, 2);
    { float z = 0; uint32_t zu; memcpy(&zu, &z, 4); EMIT(&ty, 43, F, F0, zu); }
    EMIT(&ty, 30, PVI, V4); EMIT(&ty, 28, AIN, PVI, U3); EMIT(&ty, 32, PAIN, SC_INPUT, AIN);
    EMIT(&ty, 30, PVO, V4); EMIT(&ty, 32, PVOP, SC_OUTPUT, PVO);
    EMIT(&ty, 32, PIV4, SC_INPUT, V4); EMIT(&ty, 32, POV4, SC_OUTPUT, V4);
    EMIT(&ty, 59, PAIN, GIN, SC_INPUT); EMIT(&ty, 59, PVOP, GOUT, SC_OUTPUT);
    EMIT(&dc, 71, PVI, D_BLOCK); EMIT(&dc, 72, PVI, 0, D_BUILTIN, B_POSITION);
    EMIT(&dc, 71, PVO, D_BLOCK); EMIT(&dc, 72, PVO, 0, D_BUILTIN, B_POSITION);
    uint32_t vin[32], vout[32], tt[32], pin_el[32];
    for (int u = 0; u < nuo; u++) {
        tt[u] = gs_copy_type(&e, &ty, map, uo[u].type, &next, F, V4, SI, UI, 0);
        if (!tt[u]) goto done;
        uint32_t arr = next++, pin = next++, pout = next++;
        pin_el[u] = next++;
        vin[u] = next++; vout[u] = next++;
        EMIT(&ty, 28, arr, tt[u], U3);
        EMIT(&ty, 32, pin, SC_INPUT, arr);
        EMIT(&ty, 32, pout, SC_OUTPUT, tt[u]);
        EMIT(&ty, 32, pin_el[u], SC_INPUT, tt[u]);
        EMIT(&ty, 59, pin, vin[u], SC_INPUT);
        EMIT(&ty, 59, pout, vout[u], SC_OUTPUT);
        for (int k = 0; k < uo[u].ndeco; k++) {
            if (uo[u].deco[k][1] == ~0u) { EMIT(&dc, 71, vin[u], uo[u].deco[k][0]); EMIT(&dc, 71, vout[u], uo[u].deco[k][0]); }
            else { EMIT(&dc, 71, vin[u], uo[u].deco[k][0], uo[u].deco[k][1]); EMIT(&dc, 71, vout[u], uo[u].deco[k][0], uo[u].deco[k][1]); }
        }
    }
    /* entry point e modos */
    {
        uint32_t a[80]; uint32_t na = 0;
        a[na++] = 3; a[na++] = MAIN; a[na++] = 0x6e69616d; a[na++] = 0; a[na++] = GIN; a[na++] = GOUT;
        for (int u = 0; u < nuo; u++) { a[na++] = vin[u]; a[na++] = vout[u]; }
        binst(&hd, 15, a, na);
    }
    EMIT(&hd, 16, MAIN, 22);                        /* Triangles */
    EMIT(&hd, 16, MAIN, 0, 1);                      /* Invocations 1 */
    EMIT(&hd, 16, MAIN, points ? 27 : 28);          /* OutputPoints / OutputLineStrip */
    EMIT(&hd, 16, MAIN, 26, points ? 3 : 4);        /* OutputVertices */
    /* corpo */
    EMIT(&fn, 54, VOID, MAIN, 0, FNT);
    EMIT(&fn, 248, L0);
    uint32_t pos[3]; const uint32_t ci_[3] = { I0, I1, I2 };
    for (int i = 0; i < 3; i++) {
        uint32_t ac = next++; pos[i] = next++;
        EMIT(&fn, 65, PIV4, ac, GIN, ci_[i], I0);
        EMIT(&fn, 61, V4, pos[i], ac);
    }
    int cf = cull & 1, cb = cull & 2;
    if (cf && cb) { EMIT(&fn, 253); EMIT(&fn, 56); goto build; }     /* FRONT_AND_BACK: nenhum polígono */
    if (cf || cb) {
        uint32_t x[3], y[3], ww[3];
        for (int i = 0; i < 3; i++) {
            x[i] = next++; y[i] = next++; ww[i] = next++;
            EMIT(&fn, 81, F, x[i], pos[i], 0); EMIT(&fn, 81, F, y[i], pos[i], 1); EMIT(&fn, 81, F, ww[i], pos[i], 3);
        }
#define MUL(r, a, b) do { r = next++; EMIT(&fn, 133, F, r, a, b); } while (0)
#define SUB(r, a, b) do { r = next++; EMIT(&fn, 131, F, r, a, b); } while (0)
#define ADD(r, a, b) do { r = next++; EMIT(&fn, 129, F, r, a, b); } while (0)
        uint32_t a1, a2, t1, b1, b2, t2, c1, c2, t3, m1, m2, m3, s1, det;
        MUL(a1, y[1], ww[2]); MUL(a2, y[2], ww[1]); SUB(t1, a1, a2);
        MUL(b1, y[0], ww[2]); MUL(b2, y[2], ww[0]); SUB(t2, b1, b2);
        MUL(c1, y[0], ww[1]); MUL(c2, y[1], ww[0]); SUB(t3, c1, c2);
        MUL(m1, x[0], t1); MUL(m2, x[1], t2); MUL(m3, x[2], t3);
        SUB(s1, m1, m2); ADD(det, s1, m3);
#undef MUL
#undef SUB
#undef ADD
        /* frente quando k*det > 0, k = (viewport negativo ? -1 : 1) * (CCW ? -1 : 1) */
        int k = ((cull & 8) ? -1 : 1) * ((cull & 4) ? 1 : -1);
        uint32_t kc = next++, kd = next++, c = next++, lr = next++, lm = next++;
        { float kf = (float)k; uint32_t ku; memcpy(&ku, &kf, 4); EMIT(&ty, 43, F, kc, ku); }
        EMIT(&fn, 133, F, kd, det, kc);
        EMIT(&fn, cf ? 186 : 184, B, c, kd, F0);    /* culla a frente (kd > 0) ou o verso (kd < 0) */
        EMIT(&fn, 247, lm, 0);
        EMIT(&fn, 250, c, lr, lm);
        EMIT(&fn, 248, lr); EMIT(&fn, 253);
        EMIT(&fn, 248, lm);
    }
    {
        static const int lines[4] = { 0, 1, 2, 0 };
        int nv = points ? 3 : 4;
        for (int k = 0; k < nv; k++) {
            int i = lines[k];
            uint32_t ao = next++;
            EMIT(&fn, 65, POV4, ao, GOUT, I0);
            EMIT(&fn, 62, ao, pos[i]);
            for (int u = 0; u < nuo; u++) {
                uint32_t ai = next++, v = next++;
                EMIT(&fn, 65, pin_el[u], ai, vin[u], ci_[uo[u].flat ? 0 : i]);
                EMIT(&fn, 61, tt[u], v, ai);
                EMIT(&fn, 62, vout[u], v);
            }
            EMIT(&fn, 218);
            if (points) EMIT(&fn, 219);
        }
        if (!points) EMIT(&fn, 219);
        EMIT(&fn, 253);
        EMIT(&fn, 56);
    }
build:
    if (oom) goto done;
    {
        size_t total = 5 + caps.n + hd.n + dc.n + ty.n + fn.n;
        out = malloc(total * 4);
        if (!out) goto done;
        out[0] = 0x07230203; out[1] = e.version; out[2] = 0; out[3] = next; out[4] = 0;
        size_t o = 5;
        /* ordem: capabilities, (extensões, memory model, entry point, modos), decorações, tipos, função */
        memcpy(&out[o], caps.w, caps.n * 4); o += caps.n;
        /* hd tem capabilities do VS, extensões e memory model antes do entry point (na ordem do VS) */
        memcpy(&out[o], hd.w, hd.n * 4); o += hd.n;
        memcpy(&out[o], dc.w, dc.n * 4); o += dc.n;
        memcpy(&out[o], ty.w, ty.n * 4); o += ty.n;
        memcpy(&out[o], fn.w, fn.n * 4); o += fn.n;
        *out_nw = o;
    }
done:
    free(hd.w); free(dc.w); free(ty.w); free(fn.w); free(caps.w); free(map);
    ed_free(&e);
    return out;
}

/* ------------------------------------------------------------------ índice não uniforme de imagens (v4.9.3)
 * A G76 usa o índice de uma thread só quando um array de imagens/samplers é indexado com valores
 * diferentes no mesmo warp. Para cada operação de imagem cujo descritor vem de um OpAccessChain
 * decorado NonUniform (no índice, na cadeia ou no load), a operação vai para um laço "waterfall":
 *   repete N vezes: se a thread ainda não terminou, f = broadcastFirst(índice) entre as que faltam,
 *   busca com t[f] (uniforme) e guarda o resultado se índice == f.
 * A busca fica fora do if (índice == f): dentro dele o compilador troca t[f] por t[índice].
 * Em fragment shader, ImplicitLod vira ExplicitLod com gradientes calculados antes do laço. */
#define WF_MAXUSE 8
static int is_img_op(uint32_t op) { return (op >= 87 && op <= 99) || (op >= 103 && op <= 107) || op == 100; }
static int img_mask_pos(uint32_t op) {   /* posição da máscara ImageOperands (0 = não tem) */
    switch (op) {
    case 87: case 88: case 91: case 92: case 95: case 98: return 5;
    case 89: case 90: case 93: case 94: case 96: case 97: return 6;
    case 99: return 4;
    }
    return 0;
}
static int imgop_nargs(uint32_t bit) {
    switch (bit) { case 0x1: case 0x2: case 0x8: case 0x10: case 0x20: case 0x40: case 0x80: case 0x100: case 0x200: return 1; case 0x4: return 2; }
    if (bit == 0x400 || bit == 0x800 || bit == 0x1000 || bit == 0x2000 || bit == 0x10000) return 0;
    return -1;
}

typedef struct { uint32_t *res_at; } WfDefs;

int spvemu_waterfall_needed(const uint32_t *w, size_t nw) {
    for (size_t i = 5; i < nw;) {
        uint32_t wc = WC(w[i]); if (!wc) return 0;
        if (OP(w[i]) == 71 && wc >= 3 && w[i + 2] == 5300) return 1;
        i += wc;
    }
    return 0;
}

uint32_t *spvemu_waterfall(const uint32_t *w, size_t nw, int iters, size_t *out_nw, int *count) {
    *count = 0;
    Ed e;
    oom = 0;
    if (ed_init(&e, w, nw, NULL)) return NULL;
    uint32_t bound0 = e.bound0;
    /* id -> instrução que o define (qualquer opcode com resultado em w[2]) e decorações NonUniform */
    uint32_t *defi = calloc(bound0, 4);
    uint8_t *nonu = calloc(bound0, 1);
    uint32_t *lab_final = calloc(bound0, 4);      /* rótulo de bloco -> rótulo do último pedaço depois de dividir */
    uint8_t *is_frag = NULL;
    if (!defi || !nonu || !lab_final) goto fail;
    for (uint32_t i = 0; i < e.ni; i++) {
        const uint32_t *x = I(&e, i); uint32_t op = OP(x[0]), wc = WC(x[0]);
        if (op == 71 && wc >= 3 && x[2] == 5300 && x[1] < bound0) nonu[x[1]] = 1;
        if (i >= e.i_func && wc >= 3 && (op == 61 || op == 65 || op == 66 || op == 86 || is_img_op(op) || op == 60) && x[2] < bound0) defi[x[2]] = i;
        if (op == 59 && wc >= 4 && x[2] < bound0) defi[x[2]] = i;
    }
    /* modelo: só converte ImplicitLod se TODOS os entry points são fragment */
    int all_frag = 1;
    for (uint32_t i = 0; i < e.i_func; i++) { const uint32_t *x = I(&e, i); if (OP(x[0]) == 15 && x[1] != 4) all_frag = 0; }
    uint32_t bt = t_bool(&e), it = t_int(&e);
    uint32_t c0 = c_int(&e, 0), c1 = c_int(&e, 1), cN = c_int(&e, iters), cScope = c_int(&e, 3);
    uint32_t cT = 0, cF = 0;
    { uint32_t a[] = { bt }; cT = ed_find(&e, 41, 2, a, 1); cF = ed_find(&e, 42, 2, a, 1); }
    uint32_t pbool = t_ptr(&e, 7, bt), pint = t_ptr(&e, 7, it);
    uint32_t cur_label = 0, fn_first_block = 0, func_start = 0;
    int nphi_fix = 0;
    for (uint32_t i = e.i_func; i < e.ni; i++) {
        const uint32_t *x = I(&e, i); uint32_t op = OP(x[0]), wc = WC(x[0]);
        if (op == 54) { func_start = i; fn_first_block = 0; continue; }
        if (op == 248) { cur_label = x[1]; if (!fn_first_block) fn_first_block = i; if (cur_label < bound0) lab_final[cur_label] = cur_label; continue; }
        if (!is_img_op(op) || op == 100 || !cur_label) continue;
        /* bloco com OpLoopMerge não pode ser dividido */
        int has_loopmerge = 0;
        for (uint32_t k = i + 1; k < e.ni; k++) { uint32_t o2 = OP(I(&e, k)[0]); if (o2 == 246) { has_loopmerge = 1; break; } if (o2 == 248 || o2 == 56) break; }
        if (has_loopmerge) continue;
        /* operando de imagem/sampled image */
        int has_res = op != 99;
        uint32_t sio = has_res ? x[3] : x[1];
        if (sio >= bound0 || !defi[sio]) continue;
        const uint32_t *d = I(&e, defi[sio]);
        uint32_t si_i = 0, ld_img = 0, ld_smp = 0, wrap_i = 0;
        if (OP(d[0]) == 100) {          /* OpImage: imagem extraída do sampled image (texelFetch em sampler2D) */
            wrap_i = defi[sio];
            if (d[3] >= bound0 || !defi[d[3]]) continue;
            d = I(&e, defi[d[3]]);
            sio = d[2];
        }
        if (OP(d[0]) == 86) { si_i = defi[sio]; ld_img = d[3] < bound0 ? defi[d[3]] : 0; ld_smp = d[4] < bound0 ? defi[d[4]] : 0; }
        else if (OP(d[0]) == 61) ld_img = defi[sio];
        else continue;
        /* loads vindos de access chain com índice dinâmico NonUniform */
        uint32_t X = 0, ac_img = 0, ac_smp = 0;
        uint32_t lds[2] = { ld_img, ld_smp }, *acs[2] = { &ac_img, &ac_smp };
        int bad = 0;
        for (int k = 0; k < 2; k++) {
            if (!lds[k]) continue;
            const uint32_t *l = I(&e, lds[k]);
            if (OP(l[0]) != 61 || WC(l[0]) != 4 || l[3] >= bound0 || !defi[l[3]]) continue;   /* load de variável: uniforme */
            const uint32_t *a = I(&e, defi[l[3]]);
            if (OP(a[0]) != 65 && OP(a[0]) != 66) continue;
            uint32_t dyn = 0; int ndyn = 0;
            for (uint32_t q = 4; q < WC(a[0]); q++) { int ok; const_val(&e, a[q], &ok); if (!ok) { dyn = a[q]; ndyn++; } }
            if (ndyn != 1) { if (ndyn > 1) bad = 1; continue; }
            int marked = (dyn < bound0 && nonu[dyn]) || (a[2] < bound0 && nonu[a[2]]) || (l[2] < bound0 && nonu[l[2]]);
            if (!marked) continue;
            if (X && X != dyn) { bad = 1; break; }
            X = dyn; *acs[k] = defi[l[3]];
        }
        if (bad || !X) continue;
        const uint32_t *xd = defw(&e, X);   /* o tipo do índice: int ou uint de 32 bits */
        uint32_t xt = 0;
        if (X < bound0 && defi[X]) xt = I(&e, defi[X])[1];
        else {
            /* índice definido por instrução sem resultado mapeado: procura o tipo em qualquer instrução */
            for (uint32_t k = e.i_func; k < e.ni && !xt; k++) { const uint32_t *y = I(&e, k); const uint32_t *ty; if (WC(y[0]) >= 3 && y[2] == X && (ty = defw(&e, y[1])) && OP(ty[0]) >= 19 && OP(ty[0]) <= 39) xt = y[1]; }
        }
        (void)xd;
        const uint32_t *xtw = defw(&e, xt);
        if (!xtw || OP(xtw[0]) != 21 || xtw[2] != 32) continue;
        /* conversão de ImplicitLod (fragment) */
        int implicit = op == 87 || op == 89, proj = op == 91 || op == 93;
        if (proj) continue;
        if (implicit && !all_frag) continue;
        uint32_t mpos = img_mask_pos(op), mask = (mpos && wc > mpos) ? x[mpos] : 0;
        if (mask & ~0x13fffu) continue;
        int ok_mask = 1;
        for (uint32_t bit = 1; bit && bit <= 0x10000; bit <<= 1) if ((mask & bit) && imgop_nargs(bit) < 0) ok_mask = 0;
        if (!ok_mask) continue;
        /* ---- monta o laço antes da instrução i ---- */
        Buf *p = &e.pre[i];
        uint32_t rt = has_res ? x[1] : 0, rid = has_res ? x[2] : 0;
        uint32_t vdone = ed_id(&e), vctr = ed_id(&e), vres = 0;
        Buf *vb = &e.pre[fn_first_block + 1];
        EMIT(vb, 59, pbool, vdone, 7);
        EMIT(vb, 59, pint, vctr, 7);
        if (has_res) { vres = ed_id(&e); EMIT(vb, 59, t_ptr(&e, 7, rt), vres, 7); }
        uint32_t dx = 0, dy = 0, coord = x[4], ct = 0;
        if (implicit) {
            for (uint32_t k = 0; k < e.ni && !ct; k++) { const uint32_t *y = I(&e, k); const uint32_t *ty; if (WC(y[0]) >= 3 && y[2] == coord && (ty = defw(&e, y[1])) && OP(ty[0]) >= 19 && OP(ty[0]) <= 39) ct = y[1]; }
            if (!ct) { ct = 0; }
        }
        if (implicit && !ct) { /* tipo da coordenada desconhecido: desfaz (só variáveis já foram emitidas; inofensivas) */ continue; }
        if (implicit) { dx = ed_id(&e); dy = ed_id(&e); EMIT(p, 207, ct, dx, coord); EMIT(p, 208, ct, dy, coord); }
        uint32_t L_hdr = ed_id(&e), L_body = ed_id(&e), L_work = ed_id(&e), L_take = ed_id(&e), L_m2 = ed_id(&e),
                 L_skip = ed_id(&e), L_cont = ed_id(&e), L_merge = ed_id(&e);
        EMIT(p, 62, vdone, cF);
        EMIT(p, 62, vctr, c0);
        EMIT(p, 249, L_hdr);
        EMIT(p, 248, L_hdr);
        uint32_t cc = ed_id(&e), lt = ed_id(&e);
        EMIT(p, 61, it, cc, vctr);
        EMIT(p, 133 - 133 + 177, bt, lt, cc, cN);       /* OpSLessThan = 177 */
        EMIT(p, 246, L_merge, L_cont, 0);
        EMIT(p, 250, lt, L_body, L_merge);
        EMIT(p, 248, L_body);
        uint32_t dd = ed_id(&e), nd = ed_id(&e);
        EMIT(p, 61, bt, dd, vdone);
        EMIT(p, 168, bt, nd, dd);
        EMIT(p, 247, L_skip, 0);
        EMIT(p, 250, nd, L_work, L_skip);
        EMIT(p, 248, L_work);
        uint32_t f = ed_id(&e);
        EMIT(p, 338, xt, f, cScope, X);
        /* cópias da cadeia com f no lugar do índice */
        uint32_t new_img = 0, new_smp = 0, new_si = 0;
        uint32_t lsrc[2] = { ld_img, ld_smp }, asrc[2] = { ac_img, ac_smp }, *outs[2] = { &new_img, &new_smp };
        for (int k = 0; k < 2; k++) {
            if (!lsrc[k]) continue;
            const uint32_t *l = I(&e, lsrc[k]);
            if (!asrc[k]) { *outs[k] = l[2]; continue; }       /* load uniforme: reusa */
            const uint32_t *a = I(&e, asrc[k]);
            uint32_t na = ed_id(&e), nl = ed_id(&e);
            bput(p, (WC(a[0]) << 16) | OP(a[0])); bput(p, a[1]); bput(p, na);
            for (uint32_t q = 3; q < WC(a[0]); q++) bput(p, a[q] == X ? f : a[q]);
            EMIT(p, 61, l[1], nl, na);
            *outs[k] = nl;
        }
        uint32_t opnd = new_img;
        if (si_i) { const uint32_t *s = I(&e, si_i); new_si = ed_id(&e); EMIT(p, 86, s[1], new_si, new_img, new_smp); opnd = new_si; }
        if (wrap_i) { const uint32_t *wi = I(&e, wrap_i); uint32_t ni_ = ed_id(&e); EMIT(p, 100, wi[1], ni_, opnd); opnd = ni_; }
        /* a operação, com o operando trocado (e gradientes se era ImplicitLod) */
        uint32_t nr = has_res ? ed_id(&e) : 0;
        {
            uint32_t nop = op == 87 ? 88 : op == 89 ? 90 : op;
            uint32_t tmp[64]; uint32_t n = 0;
            uint32_t fixed = mpos ? mpos : wc;
            if (fixed > wc) fixed = wc;
            for (uint32_t q = 1; q < fixed; q++) tmp[n++] = x[q];
            if (has_res) { tmp[1] = nr; tmp[2] = opnd; } else tmp[0] = opnd;
            if (mpos && (wc > mpos || implicit)) {
                uint32_t nm = implicit ? ((mask & ~0x1u) | 0x4u) : mask;
                tmp[n++] = nm;
                uint32_t q = mpos + 1;
                uint32_t bias = 0;
                for (uint32_t bit = 1; bit && bit <= 0x10000; bit <<= 1) {
                    int na_ = (mask & bit) ? imgop_nargs(bit) : 0;
                    if (bit == 0x1 && (mask & bit)) { bias = x[q]; q += 1; if (!implicit) tmp[n++] = bias; continue; }
                    if (bit == 0x4 && implicit) {
                        uint32_t gx = dx, gy = dy;
                        if (bias) {   /* bias com gradientes: escala por 2^bias */
                            uint32_t e2 = ed_id(&e), sx = ed_id(&e), sy = ed_id(&e);
                            uint32_t glsl = 0;
                            for (uint32_t k = 0; k < e.i_func && !glsl; k++) { const uint32_t *y = I(&e, k); if (OP(y[0]) == 11 && !strcmp((const char *)&y[2], "GLSL.std.450")) glsl = y[1]; }
                            const uint32_t *ctw = defw(&e, ct);
                            if (glsl && ctw) {
                                bput(p, (6u << 16) | 12); bput(p, t_float(&e)); bput(p, e2); bput(p, glsl); bput(p, 29); bput(p, bias);
                                uint32_t mop = OP(ctw[0]) == 23 ? 142 : 133;   /* VectorTimesScalar / FMul */
                                EMIT(p, mop, ct, sx, dx, e2); EMIT(p, mop, ct, sy, dy, e2);
                                gx = sx; gy = sy;
                            }
                        }
                        tmp[n++] = gx; tmp[n++] = gy;
                        if (mask & 0x4) q += 2;
                        continue;
                    }
                    for (int k = 0; k < na_; k++) tmp[n++] = x[q++];
                    if (n > 60) break;
                }
            }
            if (n > 60) goto fail;
            bput(p, ((n + 1) << 16) | nop);
            for (uint32_t k = 0; k < n; k++) bput(p, tmp[k]);
        }
        uint32_t mine = ed_id(&e);
        EMIT(p, 170, bt, mine, X, f);                     /* OpIEqual */
        EMIT(p, 247, L_m2, 0);
        EMIT(p, 250, mine, L_take, L_m2);
        EMIT(p, 248, L_take);
        if (has_res) EMIT(p, 62, vres, nr);
        EMIT(p, 62, vdone, cT);
        EMIT(p, 249, L_m2);
        EMIT(p, 248, L_m2);
        EMIT(p, 249, L_skip);
        EMIT(p, 248, L_skip);
        EMIT(p, 249, L_cont);
        EMIT(p, 248, L_cont);
        uint32_t c2 = ed_id(&e);
        EMIT(p, 128, it, c2, cc, c1);                     /* OpIAdd */
        EMIT(p, 62, vctr, c2);
        EMIT(p, 249, L_hdr);
        EMIT(p, 248, L_merge);
        if (has_res) EMIT(p, 61, rt, rid, vres);
        ed_set_rep(&e, i, NULL, 0);                       /* a operação original sai */
        if (cur_label < bound0) lab_final[cur_label] = L_merge;
        (*count)++;
        /* o resto do bloco continua no L_merge: OpPhi dos sucessores precisam do rótulo novo */
        nphi_fix = 1;
        (void)func_start;
        if (oom) break;
    }
    if (!*count || oom) goto fail;
    /* OpPhi: (valor, rótulo pai) com pai = bloco dividido -> último pedaço */
    if (nphi_fix)
        for (uint32_t i = e.i_func; i < e.ni; i++) {
            const uint32_t *x = I(&e, i);
            if (OP(x[0]) != 245) continue;
            uint32_t wc = WC(x[0]), r[128]; int ch = 0;
            if (wc > 128) continue;
            memcpy(r, x, wc * 4);
            for (uint32_t q = 4; q < wc; q += 2) if (r[q] < bound0 && lab_final[r[q]] && lab_final[r[q]] != r[q]) { r[q] = lab_final[r[q]]; ch = 1; }
            if (ch) ed_set_rep(&e, i, r, wc);
        }
    /* capabilities e versão (OpGroupNonUniformBroadcastFirst = SPIR-V 1.3) */
    int have61 = 0, have64 = 0;
    for (uint32_t i = 0; i < e.i_func; i++) { const uint32_t *x = I(&e, i); if (OP(x[0]) == 17) { if (x[1] == 61) have61 = 1; if (x[1] == 64) have64 = 1; } }
    if (!have61) EMIT(&e.pre[0], 17, 61);
    if (!have64) EMIT(&e.pre[0], 17, 64);
    if (all_frag) { int have_d = 0; for (uint32_t i = 0; i < e.i_func; i++) { const uint32_t *x = I(&e, i); if (OP(x[0]) == 17 && x[1] == 1) have_d = 1; } (void)have_d; }
    uint32_t *out = ed_build(&e, out_nw);
    if (out && out[1] < 0x00010300) out[1] = 0x00010300;
    free(defi); free(nonu); free(lab_final); free(is_frag);
    ed_free(&e);
    return out;
fail:
    free(defi); free(nonu); free(lab_final); free(is_frag);
    ed_free(&e);
    *count = 0;
    return NULL;
}

/* ------------------------------------------------------------------ FP16 (v5.7, opcional)
 * Só em módulos com entry points de fragment. Decora RelaxedPrecision (mediump: a Bifrost faz FP16
 * com o dobro da vazão) nos resultados float de contas, loads, amostras de textura e nos varyings
 * de entrada. Fica em FP32 (não decorado) tudo o que alimenta, mesmo indiretamente, operandos de
 * imagem que não são a imagem em si (coordenadas, dref, lod, gradientes, offsets) e o gl_FragDepth:
 * coordenada em FP16 faz textura grande tremer. */
static int fp_result_op(uint32_t op) {
    switch (op) {
    case 127: case 129: case 131: case 133: case 136: case 140: case 141: case 142: case 143: case 144: case 145:
    case 146: case 147: case 148: case 12: case 79: case 80: case 81: case 82: case 169: case 245: case 61:
    case 111: case 112: case 115:
        return 1;
    }
    return (op >= 87 && op <= 98);
}

uint32_t *spvemu_fp16(const uint32_t *w, size_t nw, size_t *out_nw, int *count) {
    *count = 0;
    Ed e;
    oom = 0;
    if (ed_init(&e, w, nw, NULL)) return NULL;
    uint32_t B = e.bound0;
    uint8_t *isf = calloc(B, 1), *keep = calloc(B, 1), *relaxed = calloc(B, 1);
    uint32_t *defi = calloc(B, 4), *stk = malloc(B * 4 + 16);
    uint8_t *var_keep = calloc(B, 1);
    uint32_t *base = calloc(B, 4);   /* ponteiro -> variável de origem */
    if (!isf || !keep || !relaxed || !defi || !stk || !var_keep || !base) goto fail;
    for (uint32_t i = 0; i < e.i_func; i++) {
        const uint32_t *x = I(&e, i); uint32_t op = OP(x[0]);
        if (op == 15 && x[1] != 4) goto fail;                       /* só fragment */
        if (op == 22 && x[2] == 32 && x[1] < B) isf[x[1]] = 1;      /* float32 */
        if ((op == 23 || op == 24) && x[1] < B && x[2] < B && isf[x[2]]) isf[x[1]] = 1;   /* vetor/matriz de float32 */
        if (op == 71 && WC(x[0]) >= 3 && x[2] == 0 && x[1] < B) relaxed[x[1]] = 1;          /* já RelaxedPrecision */
        if (op == 59 && x[2] < B) base[x[2]] = x[2];
    }
    for (uint32_t i = e.i_func; i < e.ni; i++) {
        const uint32_t *x = I(&e, i); uint32_t op = OP(x[0]), wc = WC(x[0]);
        if (wc >= 3 && x[2] < B && (fp_result_op(op) || op == 59 || op == 65 || op == 66 || op == 86 || op == 100 || op == 124 || op == 207 || op == 208 || op == 209)) defi[x[2]] = i;
        if (op == 59 && x[2] < B) base[x[2]] = x[2];
        if ((op == 65 || op == 66) && wc >= 4 && x[2] < B && x[3] < B) base[x[2]] = base[x[3]];
    }
    /* sementes: operandos de imagem que não são a imagem; valor gravado no gl_FragDepth */
    uint32_t sp = 0;
#define PUSH(id) do { uint32_t _i = (id); if (_i < B && !keep[_i]) { keep[_i] = 1; stk[sp++] = _i; } } while (0)
    for (uint32_t i = e.i_func; i < e.ni; i++) {
        const uint32_t *x = I(&e, i); uint32_t op = OP(x[0]), wc = WC(x[0]);
        if ((op >= 87 && op <= 99) || op == 105) {
            uint32_t first = op == 99 ? 2 : 4;                       /* depois do sampled image */
            uint32_t mpos = img_mask_pos(op);
            for (uint32_t q = first; q < wc; q++) {
                if (mpos && q == mpos) continue;                     /* máscara (literal) */
                if (op == 99 && q == 3) continue;                    /* texel gravado: pode ser FP16 */
                PUSH(x[q]);
            }
        }
        if (op == 62 && wc >= 3 && x[1] < B && var_builtin(&e, base[x[1]]) == 22) PUSH(x[2]);
    }
    /* fatia para trás: operandos das instruções que definem os ids marcados; loads de variáveis
     * marcam a variável e então todos os valores gravados nela */
    while (sp) {
        uint32_t id = stk[--sp];
        if (id >= B || !defi[id]) continue;
        const uint32_t *x = I(&e, defi[id]); uint32_t op = OP(x[0]), wc = WC(x[0]);
        if (op == 61 && wc >= 4) {
            uint32_t v = x[3] < B ? base[x[3]] : 0;
            if (v && !var_keep[v]) {
                var_keep[v] = 1;
                for (uint32_t i = e.i_func; i < e.ni; i++) {
                    const uint32_t *y = I(&e, i);
                    if (OP(y[0]) == 62 && WC(y[0]) >= 3 && y[1] < B && base[y[1]] == v) PUSH(y[2]);
                }
            }
            PUSH(x[3]);
            continue;
        }
        uint32_t from = 3;
        if (op == 12) from = 5;                                      /* ExtInst: set, instrução */
        for (uint32_t q = from; q < wc; q++) {
            if ((op == 79 && q >= 5) || (op == 81 && q >= 4) || (op == 82 && q >= 5)) break;   /* literais */
            PUSH(x[q]);
        }
    }
#undef PUSH
    /* decorações */
    Buf *dp = &e.pre[e.i_annot_end];
    int n = 0;
    for (uint32_t i = e.i_func; i < e.ni; i++) {
        const uint32_t *x = I(&e, i); uint32_t op = OP(x[0]), wc = WC(x[0]);
        if (!fp_result_op(op) || wc < 3 || x[1] >= B || x[2] >= B || !isf[x[1]] || keep[x[2]] || relaxed[x[2]]) continue;
        if (op == 61 && x[3] < B && var_keep[base[x[3]]]) continue;
        EMIT(dp, 71, x[2], 0); relaxed[x[2]] = 1; n++;
    }
    /* varyings de entrada (float) que não alimentam coordenadas */
    const uint32_t *ep = I(&e, e.ep_i);
    for (uint32_t k = e.ep_iface; k < WC(ep[0]); k++) {
        const uint32_t *v = defw(&e, ep[k]);
        if (!v || OP(v[0]) != 59 || v[3] != SC_INPUT || var_builtin(&e, ep[k]) >= 0 || var_keep[ep[k]] || relaxed[ep[k]]) continue;
        uint32_t t = ptr_pointee(&e, v[1]);
        if (t >= B || !isf[t]) continue;
        EMIT(dp, 71, ep[k], 0); n++;
    }
    if (!n || oom) goto fail;
    uint32_t *out = ed_build(&e, out_nw);
    *count = n;
    free(isf); free(keep); free(relaxed); free(defi); free(stk); free(var_keep); free(base);
    ed_free(&e);
    return out;
fail:
    free(isf); free(keep); free(relaxed); free(defi); free(stk); free(var_keep); free(base);
    ed_free(&e);
    return NULL;
}
