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
static uint32_t ed_find(Ed *e, uint32_t op, int res_at, const uint32_t *a, uint32_t na) {
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
    Buf *b = &e->pre[e->i_func];
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

/* ------------------------------------------------------------------ gl_DrawID -> push constant */
static int do_drawid(Ed *e, int off) {
    uint32_t dvar = 0, it = 0;
    const uint32_t *ep = I(e, e->ep_i);
    for (uint32_t k = e->ep_iface; k < WC(ep[0]); k++) {
        const uint32_t *v = defw(e, ep[k]);
        if (v && OP(v[0]) == 59 && v[3] == SC_INPUT && var_builtin(e, ep[k]) == B_DRAWINDEX) { dvar = ep[k]; it = ptr_pointee(e, v[1]); }
    }
    if (!dvar) return 0;
    const uint32_t *itw = defw(e, it);
    if (!itw || OP(itw[0]) != 21 || itw[2] != 32) return -1;
    /* bloco de push constant existente (no máximo um por entry point) */
    uint32_t pvar = 0, st = 0;
    for (uint32_t i = 0; i < e->i_func; i++) {
        const uint32_t *w = I(e, i);
        if (OP(w[0]) == 59 && WC(w[0]) >= 4 && w[3] == SC_PUSH) {
            if (pvar) return -1;
            pvar = w[2]; st = ptr_pointee(e, w[1]);
        }
    }
    uint32_t member;
    if (pvar) {
        const uint32_t *sw = defw(e, st);
        if (!sw || OP(sw[0]) != 30) return -1;
        uint32_t wc = WC(sw[0]);
        member = wc - 2;
        uint32_t r[64];
        if (wc + 1 > 64) return -1;
        memcpy(r, sw, wc * 4); r[wc] = it; r[0] = ((wc + 1) << 16) | 30;
        ed_set_rep(e, e->def[st], r, wc + 1);
        EMIT(&e->pre[e->i_annot_end], 72, st, member, D_OFFSET, (uint32_t)off);
    } else {
        st = ed_id(e); member = 0;
        EMIT(&e->pre[e->i_func], 30, st, it);
        EMIT(&e->pre[e->i_annot_end], 71, st, D_BLOCK);
        EMIT(&e->pre[e->i_annot_end], 72, st, 0, D_OFFSET, (uint32_t)off);
        uint32_t pt = ed_id(e);   /* ponteiro novo para o struct novo */
        EMIT(&e->pre[e->i_func], 32, pt, SC_PUSH, st);
        pvar = ed_id(e);
        EMIT(&e->pre[e->i_func], 59, pt, pvar, SC_PUSH);
        if (e->version >= 0x00010400) ed_iface_add(e, &pvar, 1);
    }
    uint32_t ppi = t_ptr(e, SC_PUSH, it), mc = c_int(e, (int32_t)member);
    int n = 0;
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
    return n ? 1 : 0;
}

uint32_t *spvemu_prerast(const uint32_t *w, size_t nw, const char *entry, int clip_loc, int clamp,
                         int drawid_off, size_t *out_nw, int *clip_nvec, unsigned *done) {
    *done = 0; *clip_nvec = 0;
    Ed e;
    oom = 0;
    if (ed_init(&e, w, nw, entry)) return NULL;
    if (e.ep_model > 3 || e.ep_model == 1) { ed_free(&e); return NULL; }
    if (clip_loc >= 0) { int r = do_clip(&e, clip_loc, clip_nvec); if (r < 0) goto fail; if (r) *done |= SPVEMU_DID_CLIP; }
    if (clamp && e.ep_model != 1) { int r = do_clamp(&e); if (r < 0) goto fail; if (r) *done |= SPVEMU_DID_CLAMP; }
    if (drawid_off >= 0 && e.ep_model == 0) { int r = do_drawid(&e, drawid_off); if (r < 0) goto fail; if (r) *done |= SPVEMU_DID_DRAWID; }
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
