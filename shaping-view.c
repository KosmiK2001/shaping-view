/* shaping-view.c — GTK3-монитор иерархии tc (HTB / ingress / ifb / mirred) в реальном времени.
 *
 * АРХИТЕКТУРА СБОРА ДАННЫХ
 * ------------------------
 * Все вызовы tc/ip выполняются ТОЛЬКО в рабочем потоке (worker_loop). Готовый
 * Snapshot публикуется в GUI через g_idle_add_full(); GUI-поток никогда не
 * блокируется на подпроцессах. Период опроса настраивается (1..10 с).
 *
 * ЛОГИКА INGRESS → IFB (auto-discovery)
 * -------------------------------------
 * tc перенаправляет входящий трафик на ifb-устройства действием mirred:
 *     tc filter add dev X parent ffff: ... action mirred egress redirect dev Y
 * Дискавери — разбор `tc -s filter show dev X parent ffff:`: каждая строка
 * "... mirred (Egress Redirect to device Y)" создаёт под устройством X дочерний
 * узел "Входящий", привязанный к ifb Y. Семантика пути — по матчу фильтра:
 *   - dst-адрес, равный адресу самого хоста -> "локальный (на хост)";
 *   - matchall (u32 0 0)                    -> "форвард"/"весь входящий";
 *   - прочее                                -> подпись вида "dst A.B.C.D".
 * Классы egress-HTB целевого ifb показываются в правой панели при выборе
 * узла "Входящий". Само ifb-устройство с верхнего уровня сайдбара СКРЫТО
 * (is_ifb + referenced) и живёт только как дочерний узел.
 *
 * Сборка: pkg-config gtk+-3.0 (см. Makefile). Интерфейс — русский (исходные
 * строки); переводы — стандартный gettext (README, флаг NLS=1).
 */

#include <gtk/gtk.h>
#include <glib/gstdio.h>
#ifdef HAVE_APPINDICATOR
# include <libappindicator/app-indicator.h>
#endif
#include <ctype.h>
#include <locale.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifdef ENABLE_NLS
# include <libintl.h>
# define _(s)  dgettext("shaping-view", (s))
# define N_(s) (s)
#else
# define _(s)  (s)
# define N_(s) (s)
#endif

#define APP_ID          "ru.kosmik.shaping-view"
#define HISTORY_LEN     60
#define DEF_INTERVAL_MS 1000
#define CARD_INDENT_PX  20

/* ============================ УТИЛИТЫ ============================ */

/* "396Kbit" -> 396000 bps (tc печатает десятичные K/M/G) */
static double parse_rate_bps(const char *tok)
{
    char *end = NULL;
    double v;

    if (!tok || !*tok) return -1.0;
    v = g_ascii_strtod(tok, &end);
    if (end == tok) return -1.0;
    if (g_str_has_prefix(end, "Tbit")) return v * 1e12;
    if (g_str_has_prefix(end, "Gbit")) return v * 1e9;
    if (g_str_has_prefix(end, "Mbit")) return v * 1e6;
    if (g_str_has_prefix(end, "Kbit") || g_str_has_prefix(end, "kbit")) return v * 1e3;
    if (g_str_has_prefix(end, "bit"))  return v;
    return v;
}

static char *fmt_rate(double bps)
{
    if (bps < 0)    return g_strdup("—");
    if (bps >= 1e9) return g_strdup_printf("%.2f Гбит/с", bps / 1e9);
    if (bps >= 1e6) return g_strdup_printf("%.1f Мбит/с", bps / 1e6);
    if (bps >= 1e3) return g_strdup_printf("%.0f Кбит/с", bps / 1e3);
    return g_strdup_printf("%.0f бит/с", bps);
}

/* палитра полос классов на графике (по порядку очередей) */
typedef struct { double r, g, b; } Rgb;
static const Rgb CLASS_PALETTE[10] = {
    {0.30, 0.69, 0.31}, {0.26, 0.55, 0.86}, {1.00, 0.76, 0.03}, {0.91, 0.30, 0.24},
    {0.61, 0.35, 0.84}, {0.00, 0.74, 0.74}, {0.94, 0.49, 0.70}, {0.55, 0.47, 0.34},
    {0.51, 0.72, 0.20}, {0.45, 0.51, 0.63}
};
#define CLASS_PAL_N (sizeof(CLASS_PALETTE) / sizeof(CLASS_PALETTE[0]))

static const Rgb *pal_color(guint idx)
{
    return &CLASS_PALETTE[idx % CLASS_PAL_N];
}

/* hex-IP из u32-матча: "c0a88901" -> "192.168.137.1" */
static char *hex_to_ip(const char *hex)
{
    guint8 b[4];
    int i;

    if (!hex || strlen(hex) != 8) return NULL;
    for (i = 0; i < 4; i++) {
        char t[3] = { hex[2 * i], hex[2 * i + 1], 0 };
        b[i] = (guint8) strtoul(t, NULL, 16);
    }
    return g_strdup_printf("%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
}

/* "1:10" -> 10 (для сортировки классов) */
static guint classid_minor(const char *classid)
{
    const char *c = classid ? strchr(classid, ':') : NULL;
    return c ? (guint) g_ascii_strtoull(c + 1, NULL, 16) : 0;
}

static gboolean name_is_ifb(const char *n)
{
    if (g_str_has_suffix(n, "_ifb")) return TRUE;
    if (!strncmp(n, "ifb", 3)) {           /* пул вида ifb0, ifb1 … */
        const char *p = n + 3;
        if (!*p) return FALSE;
        for (; *p; p++)
            if (!isdigit((unsigned char) *p)) return FALSE;
        return TRUE;
    }
    return FALSE;
}

/* сплит по пробелам/табам без пустых токенов */
static char **split_ws(const char *line)
{
    char **raw = g_strsplit_set(line, " \t\r\n", -1);
    guint n = 0;
    for (guint i = 0; raw[i]; i++) {
        if (raw[i][0]) raw[n++] = raw[i];
        else g_free(raw[i]);
    }
    raw[n] = NULL;
    return raw;
}

static int tok_find(char **t, const char *s)
{
    for (int i = 0; t[i]; i++)
        if (!strcmp(t[i], s)) return i;
    return -1;
}

static guint64 tok_u64(char **t, const char *key)
{
    int i = tok_find(t, key);
    return (i >= 0 && t[i + 1]) ? g_ascii_strtoull(t[i + 1], NULL, 10) : 0;
}

/* ============================ МОДЕЛЬ ============================ */

typedef struct {
    char    *kind;       /* htb / sfq / ingress / fq_codel …        */
    char    *handle;     /* "1:", "30:", "ffff:"                    */
    char    *parent;     /* "1:30" или NULL для root                */
    char    *backlog;    /* "0b 0p"                                 */
    gboolean is_root;
} TcQdisc;

typedef struct {
    char    *classid;    /* "1:10"                                  */
    char    *parent;     /* "1:1" или NULL для корня HTB            */
    char    *leaf;       /* "10:" или NULL                          */
    int      prio;       /* -1 если не задан                        */
    int      depth;      /* глубина в иерархии HTB (корень = 0)     */
    double   rate_bps;   /* -1 = неизвестно                         */
    double   ceil_bps;
    guint64  bytes, packets, dropped, overlimits;
    char    *backlog;    /* из leaf-qdisc, "0b 0p"                  */
    double   rate_now;   /* bps за последний интервал               */
} TcClass;

typedef struct {         /* один mirred-редирект из ingress устройства */
    char    *ifb;        /* целевое устройство                       */
    char    *desc;       /* "dst 192.168.137.1" / "весь трафик"      */
    gboolean matchall;
    gboolean is_local;   /* матч по адресу самого хоста              */
    guint64  bytes, packets;
    double   rate_now;
} TcIngressPath;

typedef struct {
    char     *name;
    gboolean  is_ifb;
    gboolean  referenced;  /* на устройство ссылается чужой mirred     */
    gboolean  is_tunnel;   /* tun/tap: sysfs speed мусор (10G)         */
    double    link_bps;    /* ёмкость линка из sysfs (мост — max)      */
    gboolean  has_root;
    gboolean  has_htb;
    gboolean  has_ingress;
    GPtrArray *qdiscs;     /* TcQdisc*                                 */
    GPtrArray *classes;    /* TcClass*  (egress-иерархия)              */
    GPtrArray *paths;      /* TcIngressPath* (только у физических)     */
} TcIface;

typedef struct {
    GPtrArray *ifaces;     /* TcIface* — все устройства с qdisc-ами    */
    GPtrArray *top;        /* TcIface* — верхний уровень сайдбара      */
    gboolean   mock;
    char      *error;      /* сообщение об ошибке сбора                */
} Snapshot;

static void tc_class_free(gpointer p)
{
    TcClass *c = p;
    if (!c) return;
    g_free(c->classid); g_free(c->parent); g_free(c->leaf); g_free(c->backlog);
    g_free(c);
}

static void tc_qdisc_free(gpointer p)
{
    TcQdisc *q = p;
    if (!q) return;
    g_free(q->kind); g_free(q->handle); g_free(q->parent); g_free(q->backlog);
    g_free(q);
}

static void tc_path_free(gpointer p)
{
    TcIngressPath *t = p;
    if (!t) return;
    g_free(t->ifb); g_free(t->desc);
    g_free(t);
}

static void tc_iface_free(gpointer p)
{
    TcIface *i = p;
    if (!i) return;
    g_free(i->name);
    g_ptr_array_unref(i->qdiscs);
    g_ptr_array_unref(i->classes);
    g_ptr_array_unref(i->paths);
    g_free(i);
}

static TcIface *snapshot_find_iface(Snapshot *s, const char *dev);

static Snapshot *snapshot_new(void)
{
    Snapshot *s = g_new0(Snapshot, 1);
    s->ifaces = g_ptr_array_new_with_free_func(tc_iface_free);
    s->top    = g_ptr_array_new();
    return s;
}

static void snapshot_free(Snapshot *s)
{
    if (!s) return;
    g_ptr_array_unref(s->top);
    g_ptr_array_unref(s->ifaces);
    g_free(s->error);
    g_free(s);
}

static TcIface *snapshot_iface(Snapshot *s, const char *dev)
{
    TcIface *i = snapshot_find_iface(s, dev);
    if (!i) {
        i = g_new0(TcIface, 1);
        i->name    = g_strdup(dev);
        i->is_ifb  = name_is_ifb(dev);
        i->qdiscs  = g_ptr_array_new_with_free_func(tc_qdisc_free);
        i->classes = g_ptr_array_new_with_free_func(tc_class_free);
        i->paths   = g_ptr_array_new_with_free_func(tc_path_free);
        g_ptr_array_add(s->ifaces, i);
    }
    return i;
}

static TcIface *snapshot_find_iface(Snapshot *s, const char *dev)
{
    if (!dev) return NULL;
    for (guint k = 0; k < s->ifaces->len; k++) {
        TcIface *x = s->ifaces->pdata[k];
        if (!strcmp(x->name, dev)) return x;
    }
    return NULL;
}

static double iface_classes_rate(const TcIface *i)
{
    double sum = 0;
    for (guint k = 0; i && k < i->classes->len; k++)
        sum += ((TcClass *) i->classes->pdata[k])->rate_now;
    return sum;
}

static double iface_paths_rate(const TcIface *i)
{
    double sum = 0;
    for (guint k = 0; i && k < i->paths->len; k++)
        sum += ((TcIngressPath *) i->paths->pdata[k])->rate_now;
    return sum;
}

static gboolean ip_in_list(const GPtrArray *ips, const char *ip)
{
    for (guint i = 0; i < ips->len; i++)
        if (!strcmp(g_ptr_array_index(ips, i), ip)) return TRUE;
    return FALSE;
}

/* бэклог класса: leaf-qdisc с parent == classid */
static const char *backlog_for_class(const TcIface *ifc, const TcClass *cls)
{
    for (guint i = 0; ifc && i < ifc->qdiscs->len; i++) {
        TcQdisc *q = g_ptr_array_index(ifc->qdiscs, i);
        if (q->parent && cls->classid && !strcmp(q->parent, cls->classid))
            return q->backlog;
    }
    return NULL;
}

/* ========================= КОЛЛЕКТОР (worker) ========================= */

typedef struct { guint64 bytes; gint64 us; gboolean have; } PrevV;

typedef struct {
    char       *tc;    /* абсолютный путь к tc (или NULL → мок)   */
    char       *ip;
    GHashTable *prev;  /* "dev|classid" / "dev|ing|ifb" -> PrevV*  */
} Collector;

static Collector *collector_new(void)
{
    Collector *c = g_new0(Collector, 1);
    c->prev = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    return c;
}

static void collector_free(Collector *c)
{
    if (!c) return;
    g_free(c->tc); g_free(c->ip);
    g_hash_table_unref(c->prev);
    g_free(c);
}

/* прирост байт -> bps; предыдущее значение хранится по ключу */
static double calc_rate(Collector *c, const char *key, guint64 bytes, gint64 now_us)
{
    PrevV *p = g_hash_table_lookup(c->prev, key);
    double r = 0;

    if (p && p->have && now_us > p->us) {
        if (bytes >= p->bytes)
            r = (double)(bytes - p->bytes) * 8.0 * 1e6 / (double)(now_us - p->us);
    }
    if (!p) {
        p = g_new0(PrevV, 1);
        g_hash_table_insert(c->prev, g_strdup(key), p);
    }
    p->bytes = bytes;
    p->us    = now_us;
    p->have  = TRUE;
    return r;
}

/* захват stdout подпроцесса (только worker-поток) */
static char *run_capture(const char *const argv[], GError **error)
{
    GSubprocess *sp = NULL;
    GBytes *out = NULL;
    char *result = NULL;

    sp = g_subprocess_newv(argv, G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                 G_SUBPROCESS_FLAGS_STDERR_SILENCE, error);
    if (!sp) return NULL;

    if (!g_subprocess_communicate(sp, NULL, NULL, &out, NULL, error)) {
        g_object_unref(sp);
        return NULL;
    }
    gsize sz = 0;
    gconstpointer data = g_bytes_get_data(out, &sz);
    result = g_strndup(data ? (const char *) data : "", sz);
    g_bytes_unref(out);
    g_object_unref(sp);
    return result;
}

static GPtrArray *get_host_ips(Collector *c)
{
    GPtrArray *ips = g_ptr_array_new_with_free_func(g_free);
    const char *const argv[] = { c->ip, "-4", "-o", "addr", "show", NULL };
    GError *err = NULL;

    char *out = run_capture(argv, &err);
    if (out) {
        char **lines = g_strsplit(out, "\n", -1);
        for (int i = 0; lines[i]; i++) {
            char **t = split_ws(lines[i]);
            int ix = tok_find(t, "inet");
            if (ix >= 0 && t[ix + 1]) {
                char **ipp = g_strsplit(t[ix + 1], "/", 2);
                if (ipp[0]) g_ptr_array_add(ips, g_strdup(ipp[0]));
                g_strfreev(ipp);
            }
            g_strfreev(t);
        }
        g_strfreev(lines);
        g_free(out);
    }
    g_clear_error(&err);
    return ips;
}

/* tun/tap-устройство? Ядро экспортирует /sys/class/net/<dev>/tun_flags */
static gboolean iface_is_tunnel(const char *dev)
{
    char *p = g_build_filename("/sys/class/net", dev, "tun_flags", NULL);
    gboolean r = g_file_test(p, G_FILE_TEST_EXISTS);
    g_free(p);
    return r;
}

/* скорость линка из sysfs, Мбит -> bps (0 = неизвестно) */
static double read_sysfs_speed(const char *dev)
{
    char *p = g_build_filename("/sys/class/net", dev, "speed", NULL);
    char *txt = NULL;
    g_file_get_contents(p, &txt, NULL, NULL);
    g_free(p);
    double mb = 0;
    if (txt) { mb = g_ascii_strtod(txt, NULL); g_free(txt); }
    return mb > 0 ? mb * 1e6 : 0.0;
}

/* «сырая» ёмкость устройства: speed; для моста — max(speed, скорости портов).
 * Вызывается ТОЛЬКО для не-туннелей: у tun sysfs speed = фиктивные 10 Гбит. */
static double raw_capacity_bps(const char *dev)
{
    double best = read_sysfs_speed(dev);
    char *brif = g_build_filename("/sys/class/net", dev, "brif", NULL);
    GDir *d = g_dir_open(brif, 0, NULL);
    if (d) {
        const char *port;
        while ((port = g_dir_read_name(d)) != NULL) {
            double s = read_sysfs_speed(port);
            if (s > best) best = s;   /* MAX: реалистичный потолок транзита */
        }
        g_dir_close(d);
    }
    g_free(brif);
    return best;
}

/* (определена ниже в демо-секции) */
static Snapshot *mock_snapshot(void);

/* --- разбор `tc -s qdisc show` (все устройства одним вызовом) --- */
static void parse_qdisc_all(Snapshot *s, const char *out)
{
    if (!out) return;
    char **lines = g_strsplit(out, "\n", -1);
    TcIface *cur_dev = NULL;
    TcQdisc *cur_q   = NULL;

    for (int li = 0; lines[li]; li++) {
        char **t = split_ws(lines[li]);
        if (!t[0]) { g_strfreev(t); continue; }

        if (!strcmp(t[0], "qdisc") && t[1] && t[2]) {
            int di = tok_find(t, "dev");
            int pi = tok_find(t, "parent");
            const char *dev = (di >= 0 && t[di + 1]) ? t[di + 1] : NULL;

            if (dev && strcmp(t[1], "noqueue") != 0 && strcmp(t[1], "noop") != 0) {
                cur_dev = snapshot_iface(s, dev);
                cur_q = g_new0(TcQdisc, 1);
                cur_q->kind    = g_strdup(t[1]);
                cur_q->handle  = g_strdup(t[2]);
                cur_q->parent  = (pi >= 0 && t[pi + 1]) ? g_strdup(t[pi + 1]) : NULL;
                cur_q->is_root = (tok_find(t, "root") >= 0);
                cur_q->backlog = g_strdup("0b 0p");
                g_ptr_array_add(cur_dev->qdiscs, cur_q);
                if (cur_q->is_root) cur_dev->has_root = TRUE;
                if (!strcmp(t[1], "htb"))     cur_dev->has_htb = TRUE;
                if (!strcmp(t[1], "ingress")) cur_dev->has_ingress = TRUE;
            } else {
                cur_dev = NULL;
                cur_q = NULL;
            }
        }
        else if (cur_q && !strcmp(t[0], "backlog") && t[1] && t[2]) {
            g_free(cur_q->backlog);
            cur_q->backlog = g_strdup_printf("%s %s", t[1], t[2]);
        }
        g_strfreev(t);
    }
    g_strfreev(lines);
}

/* --- разбор `tc -s class show dev X` --- */
static void parse_classes_dev(TcIface *ifc, const char *out)
{
    if (!out) return;
    char **lines = g_strsplit(out, "\n", -1);
    TcClass *cur = NULL;

    for (int li = 0; lines[li]; li++) {
        char **t = split_ws(lines[li]);
        if (!t[0]) { g_strfreev(t); continue; }

        if (!strcmp(t[0], "class") && t[1] && t[2]) {
            cur = g_new0(TcClass, 1);
            cur->classid = g_strdup(t[2]);
            cur->prio = -1;
            cur->rate_bps = cur->ceil_bps = -1.0;
            cur->backlog = g_strdup("0b 0p");
            for (int k = 3; t[k]; k++) {
                if (!strcmp(t[k], "parent") && t[k + 1]) cur->parent = g_strdup(t[++k]);
                else if (!strcmp(t[k], "leaf") && t[k + 1]) cur->leaf = g_strdup(t[++k]);
                else if (!strcmp(t[k], "prio") && t[k + 1]) cur->prio = atoi(t[++k]);
                else if (!strcmp(t[k], "rate") && t[k + 1]) cur->rate_bps = parse_rate_bps(t[++k]);
                else if (!strcmp(t[k], "ceil") && t[k + 1]) cur->ceil_bps = parse_rate_bps(t[++k]);
            }
            g_ptr_array_add(ifc->classes, cur);
        }
        else if (cur && !strcmp(t[0], "Sent") && t[1]) {
            cur->bytes = g_ascii_strtoull(t[1], NULL, 10);
            cur->packets = t[3] ? g_ascii_strtoull(t[3], NULL, 10) : 0;
            cur->dropped = tok_u64(t, "dropped");
            cur->overlimits = tok_u64(t, "overlimits");
        }
        else if (cur && !strcmp(t[0], "backlog") && t[1] && t[2]) {
            g_free(cur->backlog);
            cur->backlog = g_strdup_printf("%s %s", t[1], t[2]);
        }
        g_strfreev(t);
    }
    g_strfreev(lines);
}

/* --- разбор `tc -s filter show dev X parent ffff:`: ищем mirred → ifb --- */
static void parse_ingress_dev(TcIface *dev, const char *out, const GPtrArray *host_ips)
{
    if (!out) return;
    char **lines = g_strsplit(out, "\n", -1);
    TcIngressPath *cur = NULL;
    gboolean matchall = FALSE;
    char *match_ip = NULL;   /* dotted quad из hex-матча */
    int at_off = -1;         /* смещение u32: 16 = dst, 12 = src */

    for (int li = 0; lines[li]; li++) {
        char **t = split_ws(lines[li]);
        if (!t[0]) { g_strfreev(t); continue; }

        if (!strcmp(t[0], "filter")) {
            /* новый фильтр — сброс контекста матча */
            matchall = FALSE;
            at_off = -1;
            g_free(match_ip); match_ip = NULL;
            cur = NULL;
        }
        else if (!strcmp(t[0], "match") && t[1]) {
            if (!strcmp(t[1], "u32")) {
                matchall = (t[2] && t[3] && !strcmp(t[2], "0") && !strcmp(t[3], "0"));
                at_off = t[5] ? atoi(t[5]) : -1;
                g_free(match_ip); match_ip = NULL;
            } else if (strchr(t[1], '/')) {
                char **vm = g_strsplit(t[1], "/", 2);
                g_free(match_ip);
                match_ip = hex_to_ip(vm[0]);
                matchall = FALSE;
                at_off = t[3] ? atoi(t[3]) : -1;
                g_strfreev(vm);
            }
        }
        else if (strstr(lines[li], "mirred (Egress Redirect to device ")) {
            const char *p = strstr(lines[li], "to device ");
            const char *e = p ? strchr(p, ')') : NULL;
            if (p && e && e > p) {
                cur = g_new0(TcIngressPath, 1);
                cur->ifb = g_strndup(p + strlen("to device "), (gsize)(e - (p + strlen("to device "))));
                cur->matchall = matchall;
                if (matchall) {
                    cur->desc = g_strdup("весь трафик");
                } else if (match_ip) {
                    cur->desc = g_strdup_printf("%s %s",
                            at_off == 16 ? "dst" : (at_off == 12 ? "src" : "match"),
                            match_ip);
                    if (at_off == 16 && ip_in_list(host_ips, match_ip))
                        cur->is_local = TRUE;
                } else {
                    cur->desc = g_strdup("матч");
                }
                g_ptr_array_add(dev->paths, cur);
            }
        }
        else if (cur && !strcmp(t[0], "Sent") && t[1]) {
            cur->bytes = g_ascii_strtoull(t[1], NULL, 10);
            cur->packets = t[3] ? g_ascii_strtoull(t[3], NULL, 10) : 0;
        }
        g_strfreev(t);
    }
    g_free(match_ip);
    g_strfreev(lines);
}

/* --- глубина HTB-иерархии и сортировка --- */
static void assign_depth(GPtrArray *classes)
{
    for (guint i = 0; i < classes->len; i++) {
        TcClass *c = g_ptr_array_index(classes, i);
        int d = 0;
        TcClass *p = c;
        while (p && p->parent && d < 16) {
            TcClass *pp = NULL;
            for (guint j = 0; j < classes->len; j++) {
                TcClass *x = g_ptr_array_index(classes, j);
                if (!strcmp(x->classid, p->parent)) { pp = x; break; }
            }
            if (!pp || pp == p) break;
            d++;
            p = pp;
        }
        c->depth = d;
    }
}

static int class_cmp(gconstpointer a, gconstpointer b)
{
    const TcClass *x = *(const TcClass * const *) a;
    const TcClass *y = *(const TcClass * const *) b;
    if (x->depth != y->depth) return x->depth - y->depth;
    guint mx = classid_minor(x->classid), my = classid_minor(y->classid);
    if (mx != my) return mx < my ? -1 : 1;
    return strcmp(x->classid ? x->classid : "", y->classid ? y->classid : "");
}

static int iface_cmp(gconstpointer a, gconstpointer b)
{
    const TcIface *x = *(const TcIface * const *) a;
    const TcIface *y = *(const TcIface * const *) b;
    return g_utf8_collate(x->name, y->name);
}

static Snapshot *collect_snapshot(Collector *c)
{
    Snapshot *s = snapshot_new();
    gint64 now_us = g_get_monotonic_time();

    if (!c->tc) { snapshot_free(s); return mock_snapshot(); }

    /* 1) адреса хоста — для эвристики «локальный ingress» */
    GPtrArray *ips = get_host_ips(c);

    /* 2) все qdisc-ы одним вызовом */
    const char *const a1[] = { c->tc, "-s", "qdisc", "show", NULL };
    char *out = run_capture(a1, NULL);
    parse_qdisc_all(s, out);
    g_free(out);

    /* 3) классы и ingress-фильтры по устройствам */
    for (guint i = 0; i < s->ifaces->len; i++) {
        TcIface *ifc = g_ptr_array_index(s->ifaces, i);
        if (ifc->has_htb) {
            const char *const av[] = { c->tc, "-s", "class", "show", "dev", ifc->name, NULL };
            out = run_capture(av, NULL);
            parse_classes_dev(ifc, out);
            g_free(out);
        }
        if (ifc->has_ingress) {
            const char *const av[] = { c->tc, "-s", "filter", "show", "dev", ifc->name, "parent", "ffff:", NULL };
            out = run_capture(av, NULL);
            parse_ingress_dev(ifc, out, ips);
            g_free(out);
        }
    }
    g_ptr_array_unref(ips);

    /* 4) скорости по приросту байт */
    for (guint i = 0; i < s->ifaces->len; i++) {
        TcIface *ifc = g_ptr_array_index(s->ifaces, i);
        for (guint k = 0; k < ifc->classes->len; k++) {
            TcClass *cl = g_ptr_array_index(ifc->classes, k);
            char *key = g_strdup_printf("%s|%s", ifc->name, cl->classid);
            cl->rate_now = calc_rate(c, key, cl->bytes, now_us);
            g_free(key);
        }
        for (guint k = 0; k < ifc->paths->len; k++) {
            TcIngressPath *p = g_ptr_array_index(ifc->paths, k);
            char *key = g_strdup_printf("%s|ing|%s", ifc->name, p->ifb);
            p->rate_now = calc_rate(c, key, p->bytes, now_us);
            g_free(key);
        }
    }

    /* 5) глубина классов + сортировка */
    for (guint i = 0; i < s->ifaces->len; i++) {
        TcIface *ifc = g_ptr_array_index(s->ifaces, i);
        assign_depth(ifc->classes);
        if (ifc->classes->len > 1)
            g_ptr_array_sort(ifc->classes, class_cmp);
    }

    /* 6) пометка referenced: ifb, на которые есть mirred-ссылки */
    for (guint i = 0; i < s->ifaces->len; i++) {
        TcIface *ifc = g_ptr_array_index(s->ifaces, i);
        for (guint k = 0; k < ifc->paths->len; k++) {
            TcIngressPath *p = g_ptr_array_index(ifc->paths, k);
            TcIface *tgt = snapshot_iface(s, p->ifb);
            if (tgt) tgt->referenced = TRUE;
        }
    }

    /* 6b) тип устройства и ёмкость линка (sysfs; туннелям speed не верим) */
    for (guint i = 0; i < s->ifaces->len; i++) {
        TcIface *ifc = g_ptr_array_index(s->ifaces, i);
        ifc->is_tunnel = iface_is_tunnel(ifc->name);
        ifc->link_bps = ifc->is_tunnel ? 0.0 : raw_capacity_bps(ifc->name);
    }

    /* 7) верхний уровень: физические устройства с содержимым + автономные ifb */
    for (guint i = 0; i < s->ifaces->len; i++) {
        TcIface *ifc = g_ptr_array_index(s->ifaces, i);
        gboolean interesting = (ifc->classes->len > 0 || ifc->paths->len > 0 || ifc->has_ingress);
        if (ifc->is_ifb) {
            if (ifc->referenced) continue;       /* скрыт как дочерний узел */
            if (!interesting) continue;          /* автономный ifb без шейпинга */
        } else if (!interesting) {
            continue;
        }
        g_ptr_array_add(s->top, ifc);
    }
    g_ptr_array_sort(s->top, iface_cmp);

    return s;
}

/* --- мок: если tc недоступен, интерфейс всё равно живой --- */
static TcClass *mock_class(TcIface *i, const char *id, const char *parent,
                           const char *leaf, int prio, double rate, double ceil)
{
    TcClass *cl = g_new0(TcClass, 1);
    cl->classid = g_strdup(id);
    cl->parent = parent ? g_strdup(parent) : NULL;
    cl->leaf = leaf ? g_strdup(leaf) : NULL;
    cl->prio = prio;
    cl->rate_bps = rate; cl->ceil_bps = ceil;
    cl->backlog = g_strdup("0b 0p");
    cl->rate_now = rate;
    g_ptr_array_add(i->classes, cl);
    return cl;
}

static void mock_path(TcIface *i, const char *ifb, gboolean matchall,
                      const char *desc, gboolean is_local, double rate)
{
    TcIngressPath *p = g_new0(TcIngressPath, 1);
    p->ifb = g_strdup(ifb);
    p->desc = g_strdup(desc);
    p->matchall = matchall;
    p->is_local = is_local;
    p->rate_now = rate;
    g_ptr_array_add(i->paths, p);
}

static Snapshot *mock_snapshot(void)
{
    static gint64 t0 = 0;
    if (!t0) t0 = g_get_monotonic_time();
    double t = (g_get_monotonic_time() - t0) / 1e6;

    Snapshot *s = snapshot_new();
    s->mock = TRUE;

    double r_wan = 40e6 + 28e6 * fabs(sin(t * 0.55)) + 4e6 * sin(t * 2.1);
    double r_lan = 22e6 + 14e6 * fabs(sin(t * 0.37 + 1.0));
    double r_loc = 4e6  + 2.5e6 * fabs(sin(t * 0.8 + 0.5));
    double r_dns = 250e3 * (0.5 + 0.5 * sin(t * 3.0));

    TcIface *wan = snapshot_iface(s, "eth0-demo");
    wan->has_root = wan->has_htb = TRUE;
    mock_class(wan, "1:1",  NULL,  NULL,  0, 99e6, 99e6)->rate_now = r_wan;
    mock_class(wan, "1:10", "1:1", "10:", 1, 396e3, 594e3)->rate_now = r_dns;
    mock_class(wan, "1:20", "1:1", "20:", 2, 95e6, 97e6)->rate_now = r_wan * 0.75;
    mock_class(wan, "1:30", "1:1", "30:", 3, 12e6, 89e6)->rate_now = r_wan * 0.1;
    mock_class(wan, "1:70", "1:1", "70:", 7, 1e6, 89e6)->rate_now = r_wan * 0.05;
    mock_path(wan, "wan0_ifb-demo", TRUE, "весь трафик", FALSE, r_wan);

    TcIface *wifb = snapshot_iface(s, "wan0_ifb-demo");
    wifb->has_root = wifb->has_htb = TRUE;
    mock_class(wifb, "1:1",  NULL,  NULL,  0, 99e6, 99e6)->rate_now = r_wan;
    mock_class(wifb, "1:20", "1:1", "20:", 2, 95e6, 97e6)->rate_now = r_wan * 0.7;
    mock_class(wifb, "1:60", "1:1", "60:", 6, 19e6, 94e6)->rate_now = r_wan * 0.2;

    TcIface *lan = snapshot_iface(s, "brlan0-demo");
    lan->has_root = lan->has_htb = lan->has_ingress = TRUE;
    mock_class(lan, "1:1", NULL, NULL, 0, 985e6, 985e6)->rate_now = r_lan;
    mock_path(lan, "brlan0_loc_ifb-demo", FALSE, "dst 192.168.1.1", TRUE, r_loc);
    mock_path(lan, "brlan0_fwr_ifb-demo", TRUE, "весь трафик", FALSE, r_lan);

    TcIface *lifb = snapshot_iface(s, "brlan0_loc_ifb-demo");
    lifb->has_root = lifb->has_htb = TRUE;
    mock_class(lifb, "1:1",  NULL,  NULL,  0, 100e6, 100e6)->rate_now = r_loc;
    mock_class(lifb, "1:30", "1:1", "30:", 3, 50e6, 99e6)->rate_now = r_loc * 0.8;
    mock_class(lifb, "1:40", "1:1", "40:", 4, 50e6, 98e6)->rate_now = r_loc * 0.2;

    TcIface *fifb = snapshot_iface(s, "brlan0_fwr_ifb-demo");
    fifb->has_root = fifb->has_htb = TRUE;
    mock_class(fifb, "1:1",  NULL,  NULL,  0, 100e6, 100e6)->rate_now = r_lan;
    mock_class(fifb, "1:30", "1:1", "30:", 3, 50e6, 99e6)->rate_now = r_lan * 0.85;
    mock_class(fifb, "1:40", "1:1", "40:", 4, 50e6, 98e6)->rate_now = r_lan * 0.15;

    /* referenced: ifb скрыть с верхнего уровня */
    snapshot_find_iface(s, "wan0_ifb-demo")->referenced = TRUE;
    snapshot_find_iface(s, "brlan0_loc_ifb-demo")->referenced = TRUE;
    snapshot_find_iface(s, "brlan0_fwr_ifb-demo")->referenced = TRUE;

    /* top: только физические */
    g_ptr_array_add(s->top, wan);
    g_ptr_array_add(s->top, lan);

    for (guint i = 0; i < s->ifaces->len; i++) {
        TcIface *ifc = g_ptr_array_index(s->ifaces, i);
        assign_depth(ifc->classes);
        if (ifc->classes->len > 1)
            g_ptr_array_sort(ifc->classes, class_cmp);
    }
    return s;
}

/* ======================= ПАКИ САЙДБАРА ========================= */

typedef enum { PACK_HW, PACK_VPNSRV, PACK_VPNCLI, PACK_N } PackKind;

static const char *PACK_KEY[PACK_N]   = { "@hw", "@vpnserver", "@vpncli" };
static const char *PACK_TITLE[PACK_N] = { N_("Реальное железо"),
                                          N_("VPN-серверы"),
                                          N_("VPN-клиенты") };

/* =============================== UI =============================== */

typedef struct {
    GtkLabel       *rate;
    GtkProgressBar *bar;
    GtkLabel       *pct;
    GtkLabel       *st_drops;
    GtkLabel       *st_over;
    GtkLabel       *st_back;
} CardRefs;

typedef struct { double v[HISTORY_LEN]; int len; double rmax; } History;

typedef struct {
    GtkApplication *app;
    GtkWidget      *win;
    GtkWidget      *header;
    GtkWidget      *btn_pause;
    GtkWidget      *spin;
    GtkLabel       *status;
    GtkTreeStore   *store;
    GtkWidget      *tree;
    GtkWidget      *cards_box;
    GtkLabel       *info;
    GtkWidget      *graph;
    GtkWidget      *vpaned;      /* вертикальный сплит: списки ↔ график  */
    GtkWidget      *hp;          /* верхняя панель (дерево + карточки)   */
    GtkWidget      *btn_expand;  /* «во всю высоту»                      */

    GThread        *worker;
    gint           worker_run;
    gint           paused;
    gint           interval_ms;
    char           *tc_path;
    char           *ip_path;

    Snapshot       *cur;         /* текущий применённый снапшот          */
    GHashTable     *cards;       /* classid -> CardRefs*                 */
    char           *cards_dev;   /* устройство, чьи карточки показаны    */
    guint          cards_count;
    GHashTable     *hist;        /* ключ сайдбара -> History*            */
    char           *sel_key;
    GKeyFile       *names;       /* [dev] classid = Имя                  */
    GPtrArray      *grp[PACK_N]; /* [groups]: glob-паттерны паков        */

    /* трей: StatusNotifier (libappindicator) или XEmbed GtkStatusIcon */
#ifdef HAVE_APPINDICATOR
    AppIndicator   *tray;
#else
    GtkStatusIcon  *tray;
#endif
    GtkWidget      *tray_menu;
    GtkCheckMenuItem *mi_pause;
} App;

static App APP;

enum { COL_KEY, COL_NAME, COL_BADGE, COL_RATE, N_COLS };

/* --- разбор ключа сайдбара: "dev" | "dev|egress" | "dev|ing|ifb" --- */
static void parse_key(const char *key, char **dev, char **ifb, gboolean *is_ing)
{
    *dev = *ifb = NULL;
    *is_ing = FALSE;
    const char *p1 = key ? strchr(key, '|') : NULL;
    if (!p1) { *dev = g_strdup(key); return; }
    *dev = g_strndup(key, (gsize)(p1 - key));
    if (!strncmp(p1, "|ing|", 5)) {
        *is_ing = TRUE;
        *ifb = g_strdup(p1 + 5);
    }
}

/* --- классификация устройства по пакам --- */

static gboolean match_pattern_list(const char *dev, GPtrArray *patterns)
{
    if (!patterns) return FALSE;
    for (guint i = 0; i < patterns->len; i++)
        if (g_pattern_match_simple(g_ptr_array_index(patterns, i), dev)) return TRUE;
    return FALSE;
}

/* порядок правил: конфиг [groups] → признак туннеля → эвристика имени → железо */
static PackKind classify_iface(const TcIface *ifc)
{
    const char *dev = ifc->name;

    if (match_pattern_list(dev, APP.grp[PACK_VPNSRV])) return PACK_VPNSRV;
    if (match_pattern_list(dev, APP.grp[PACK_VPNCLI])) return PACK_VPNCLI;
    if (match_pattern_list(dev, APP.grp[PACK_HW]))     return PACK_HW;

    if (ifc->is_tunnel) {
        if (g_str_has_prefix(dev, "tap") ||
            strstr(dev, "server") || strstr(dev, "srv"))
            return PACK_VPNSRV;
        return PACK_VPNCLI;
    }
    return PACK_HW;
}

/* агрегат пака: сумма rate_now всех его устройств (egress + ingress-пути) */
static double rate_for_pack(Snapshot *s, PackKind pk)
{
    double r = 0;
    for (guint i = 0; i < s->top->len; i++) {
        TcIface *ifc = g_ptr_array_index(s->top, i);
        if (classify_iface(ifc) == pk)
            r += iface_classes_rate(ifc) + iface_paths_rate(ifc);
    }
    return r;
}

/* корневой класс HTB устройства (parent == NULL) */
static TcClass *root_class_of(const TcIface *ifc)
{
    for (guint i = 0; ifc && i < ifc->classes->len; i++) {
        TcClass *cl = g_ptr_array_index(ifc->classes, i);
        if (!cl->parent) return cl;
    }
    return NULL;
}

/* физическая основа для VPN: первое «железное» устройство с корнем или линком */
static TcIface *uplink_iface(Snapshot *s)
{
    for (guint i = 0; s && i < s->top->len; i++) {
        TcIface *ifc = g_ptr_array_index(s->top, i);
        if (ifc->is_ifb || ifc->is_tunnel) continue;
        if (classify_iface(ifc) != PACK_HW) continue;
        TcClass *rc = root_class_of(ifc);
        if ((rc && rc->rate_bps > 0) || ifc->link_bps > 0) return ifc;
    }
    return NULL;
}

/* шкала графика узла. Приоритет: rate корня → link → аплинк; 0 = «по счётчикам».
 * src_label — короткое имя источника для подписи. */
static double scale_for_node(Snapshot *s, const char *key, char **src_label)
{
    char *dev = NULL, *ifb = NULL;
    gboolean is_ing = FALSE;
    double v = 0;

    parse_key(key, &dev, &ifb, &is_ing);
    /* для ingress-узла базой является родительское устройство */
    TcIface *ifc = s ? snapshot_find_iface(s, is_ing ? ifb : dev) : NULL;
    TcIface *base = (is_ing && dev) ? snapshot_find_iface(s, dev) : ifc;
    TcClass *rc = root_class_of(base);

    if (rc && rc->rate_bps > 0) {
        if (src_label) *src_label = g_strdup("rate 1:1");
        v = rc->rate_bps;
    } else if (base && base->link_bps > 0) {
        if (src_label) *src_label = g_strdup("link");
        v = base->link_bps;
    } else {
        TcIface *up = uplink_iface(s);
        TcClass *urc = up ? root_class_of(up) : NULL;
        if (up && urc && urc->rate_bps > 0) {
            if (src_label) *src_label = g_strdup_printf("uplink %s", up->name);
            v = urc->rate_bps;
        } else if (up && up->link_bps > 0) {
            if (src_label) *src_label = g_strdup_printf("uplink %s (link)", up->name);
            v = up->link_bps;
        } else if (src_label) {
            *src_label = g_strdup("по счётчикам");
        }
    }
    g_free(dev); g_free(ifb);
    return v;
}

static double rate_for_key(Snapshot *s, const char *key)
{
    char *dev = NULL, *ifb = NULL;
    gboolean is_ing = FALSE;
    double r = 0;

    /* ключи паков — агрегат по участникам */
    if (key && key[0] == '@') {
        PackKind pk = PACK_HW;
        if      (!strcmp(key, PACK_KEY[PACK_VPNSRV])) pk = PACK_VPNSRV;
        else if (!strcmp(key, PACK_KEY[PACK_VPNCLI])) pk = PACK_VPNCLI;
        return rate_for_pack(s, pk);
    }

    parse_key(key, &dev, &ifb, &is_ing);
    TcIface *ifc = snapshot_find_iface(s, dev);
    if (is_ing) {
        if (ifc)
            for (guint k = 0; k < ifc->paths->len; k++) {
                TcIngressPath *p = g_ptr_array_index(ifc->paths, k);
                if (ifb && !strcmp(p->ifb, ifb)) r += p->rate_now;
            }
    } else if (ifc) {
        r += iface_classes_rate(ifc);
        if (!strstr(key, "|egress")) r += iface_paths_rate(ifc);
    }
    g_free(dev); g_free(ifb);
    return r;
}

static void hist_push(const char *key, double r)
{
    History *h = g_hash_table_lookup(APP.hist, key);
    if (!h) {
        h = g_new0(History, 1);
        g_hash_table_insert(APP.hist, g_strdup(key), h);
    }
    if (h->len < HISTORY_LEN) h->v[h->len++] = r;
    else {
        memmove(h->v, h->v + 1, sizeof(double) * (HISTORY_LEN - 1));
        h->v[HISTORY_LEN - 1] = r;
    }
    if (r > h->rmax) h->rmax = r;
    if (h->len == HISTORY_LEN) {   /* кольцо переполнилось — пересчёт окна */
        double mx = 0;
        for (int i = 0; i < h->len; i++)
            if (h->v[i] > mx) mx = h->v[i];
        h->rmax = mx;
    }
}

static History *hist_get(const char *key)
{
    if (!key) return NULL;
    return g_hash_table_lookup(APP.hist, key);
}

/* значение истории в слоте кольца 0..HISTORY_LEN-1 (справа выровнено) */
static double hist_value_at(History *h, int slot)
{
    if (!h || slot < HISTORY_LEN - h->len) return 0.0;
    return h->v[slot - (HISTORY_LEN - h->len)];
}

static char *ingress_child_label(const TcIngressPath *p, guint npaths)
{
    if (p->is_local) return g_strdup("Входящий: локальный (на хост)");
    if (p->matchall) return g_strdup(npaths > 1 ? "Входящий: форвард (остальное)"
                                                : "Входящий: весь входящий");
    return g_strdup_printf("Входящий: %s", p->desc ? p->desc : "?");
}

static char *auto_tag(const TcClass *cl)
{
    GString *g = g_string_new(NULL);
    if (cl->prio >= 0) g_string_append_printf(g, "prio %d", cl->prio);
    if (cl->leaf) {
        if (g->len) g_string_append(g, " · ");
        g_string_append_printf(g, "leaf %s", cl->leaf);
    }
    if (!g->len) g_string_append(g, "класс HTB");
    return g_string_free(g, FALSE);
}

/* --- дерево: поиск/вставка/чистка строк --- */

static gboolean find_row_by_key(GtkTreeStore *st, GtkTreeIter *parent,
                                const char *key, GtkTreeIter *out)
{
    GtkTreeModel *m = GTK_TREE_MODEL(st);
    GtkTreeIter it;
    gboolean ok = parent ? gtk_tree_model_iter_children(m, &it, parent)
                         : gtk_tree_model_get_iter_first(m, &it);
    while (ok) {
        char *k = NULL;
        gtk_tree_model_get(m, &it, COL_KEY, &k, -1);
        gboolean eq = (k && !strcmp(k, key));
        g_free(k);
        if (eq) { *out = it; return TRUE; }
        ok = gtk_tree_model_iter_next(m, &it);
    }
    return FALSE;
}

static void upsert_row(const char *key, const char *name, const char *badge,
                       GtkTreeIter *parent, GtkTreeIter *out, gboolean *created)
{
    if (find_row_by_key(APP.store, parent, key, out)) {
        if (created) *created = FALSE;
        return;
    }
    gtk_tree_store_append(APP.store, out, parent);
    gtk_tree_store_set(APP.store, out,
                       COL_KEY, key, COL_NAME, name,
                       COL_BADGE, badge, COL_RATE, "—", -1);
    if (created) *created = TRUE;
}

static void remove_stale(GtkTreeIter *parent, GHashTable *present)
{
    GtkTreeModel *m = GTK_TREE_MODEL(APP.store);
    GtkTreeIter it;
    gboolean ok = parent ? gtk_tree_model_iter_children(m, &it, parent)
                         : gtk_tree_model_get_iter_first(m, &it);
    while (ok) {
        char *k = NULL;
        gtk_tree_model_get(m, &it, COL_KEY, &k, -1);
        GtkTreeIter victim = it;
        ok = gtk_tree_model_iter_next(m, &it);
        if (!k || !g_hash_table_contains(present, k))
            gtk_tree_store_remove(APP.store, &victim);
        g_free(k);
    }
}

/* --- карточки классов --- */

static void cb_destroy_widget(GtkWidget *w, gpointer data)
{
    (void) data;
    gtk_widget_destroy(w);
}

static void cards_clear(void)
{
    gtk_container_foreach(GTK_CONTAINER(APP.cards_box), cb_destroy_widget, NULL);
    g_hash_table_remove_all(APP.cards);
    g_free(APP.cards_dev);
    APP.cards_dev = NULL;
    APP.cards_count = 0;
}

static char *class_display_name(const char *dev, const char *classid)
{
    if (!APP.names) return NULL;
    return g_key_file_get_string(APP.names, dev, classid, NULL);
}

static GtkWidget *make_card(const char *dev, TcClass *cl, guint cidx)
{
    GtkWidget *frame = gtk_frame_new(NULL);
    gtk_frame_set_shadow_type(GTK_FRAME(frame), GTK_SHADOW_ETCHED_IN);

    /* визуальная вложенность HTB-иерархии */
    gtk_widget_set_margin_start(frame, cl->depth * CARD_INDENT_PX);

    /* заголовок: цветной маркер (цвет полосы на графике) + classid + имя/тег */
    const Rgb *col = pal_color(cidx);
    char *colhex = g_strdup_printf("#%02x%02x%02x",
                                   (unsigned)(col->r * 255),
                                   (unsigned)(col->g * 255),
                                   (unsigned)(col->b * 255));
    GtkWidget *lh = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    char *m1 = g_strdup_printf("<span foreground='%s'>●</span> <b>%s</b>", colhex, cl->classid);
    GtkWidget *l_id = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(l_id), m1);
    gtk_box_pack_start(GTK_BOX(lh), l_id, FALSE, FALSE, 0);

    char *nm = class_display_name(dev, cl->classid);
    char *tag = nm ? g_strdup(nm) : auto_tag(cl);
    char *m2 = g_strdup_printf("<span foreground='#607d8b' size='small'>%s</span>", tag);
    GtkWidget *l_tag = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(l_tag), m2);
    gtk_box_pack_end(GTK_BOX(lh), l_tag, FALSE, FALSE, 0);
    gtk_frame_set_label_widget(GTK_FRAME(frame), lh);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 5);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 6);

    CardRefs *cr = g_new0(CardRefs, 1);

    /* строка 1: битрейт · полоса · процент — табличные столбцы одной ширины
       у всех карточек (в т.ч. с отступом глубины), вид таблицы */
    char *rt = fmt_rate(cl->rate_now);
    char *ct = cl->ceil_bps > 0 ? fmt_rate(cl->ceil_bps) : g_strdup("—");
    char *m3 = g_strdup_printf("<b>%s</b> <span foreground='#78909c'>/ %s</span>", rt, ct);
    cr->rate = GTK_LABEL(gtk_label_new(NULL));
    gtk_label_set_markup(cr->rate, m3);
    gtk_label_set_xalign(cr->rate, 0.0);
    gtk_label_set_width_chars(cr->rate, 19);
    gtk_label_set_max_width_chars(cr->rate, 19);
    gtk_label_set_ellipsize(cr->rate, PANGO_ELLIPSIZE_END);
    gtk_grid_attach(GTK_GRID(grid), GTK_WIDGET(cr->rate), 0, 0, 1, 1);

    cr->bar = GTK_PROGRESS_BAR(gtk_progress_bar_new());
    gtk_progress_bar_set_show_text(cr->bar, FALSE);
    gtk_widget_set_size_request(GTK_WIDGET(cr->bar), 240, -1);
    gtk_grid_attach(GTK_GRID(grid), GTK_WIDGET(cr->bar), 1, 0, 1, 1);

    cr->pct = GTK_LABEL(gtk_label_new("0%"));
    gtk_label_set_xalign(cr->pct, 1.0);
    gtk_label_set_width_chars(cr->pct, 5);
    gtk_style_context_add_class(gtk_widget_get_style_context(GTK_WIDGET(cr->pct)), "mono");
    gtk_grid_attach(GTK_GRID(grid), GTK_WIDGET(cr->pct), 2, 0, 1, 1);

    /* строка 2: чипы статистики — чёрный скруглённый фон, текст белый,
       числа: дропы красным, оверлимиты оранжевым, бэклог синим */
    GtkWidget *chips = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    cr->st_drops = GTK_LABEL(gtk_label_new(NULL));
    cr->st_over  = GTK_LABEL(gtk_label_new(NULL));
    cr->st_back  = GTK_LABEL(gtk_label_new(NULL));
    gtk_style_context_add_class(gtk_widget_get_style_context(GTK_WIDGET(cr->st_drops)), "chip");
    gtk_style_context_add_class(gtk_widget_get_style_context(GTK_WIDGET(cr->st_over)), "chip");
    gtk_style_context_add_class(gtk_widget_get_style_context(GTK_WIDGET(cr->st_back)), "chip");
    gtk_box_pack_start(GTK_BOX(chips), GTK_WIDGET(cr->st_drops), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(chips), GTK_WIDGET(cr->st_over), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(chips), GTK_WIDGET(cr->st_back), FALSE, FALSE, 0);
    gtk_grid_attach(GTK_GRID(grid), chips, 0, 1, 3, 1);

    gtk_container_add(GTK_CONTAINER(frame), grid);
    g_hash_table_insert(APP.cards, g_strdup(cl->classid), cr);

    g_free(m1); g_free(m2); g_free(m3); g_free(rt); g_free(ct); g_free(tag); g_free(nm);
    g_free(colhex);
    return frame;
}

static void update_cards(Snapshot *s)
{
    if (!APP.cards_dev || !s) return;
    TcIface *ifc = snapshot_find_iface(s, APP.cards_dev);
    if (!ifc) return;

    GHashTableIter it;
    gpointer k, v;
    g_hash_table_iter_init(&it, APP.cards);
    while (g_hash_table_iter_next(&it, &k, &v)) {
        const char *classid = k;
        CardRefs *cr = v;
        TcClass *cl = NULL;
        for (guint i = 0; i < ifc->classes->len; i++) {
            TcClass *x = g_ptr_array_index(ifc->classes, i);
            if (!strcmp(x->classid, classid)) { cl = x; break; }
        }
        if (!cl) continue;

        double frac = (cl->ceil_bps > 0) ? cl->rate_now / cl->ceil_bps : 0.0;
        if (frac < 0) frac = 0;
        if (frac > 1) frac = 1;
        gtk_progress_bar_set_fraction(cr->bar, frac);
        char *pct = g_strdup_printf("%.0f%%", frac * 100.0);
        gtk_label_set_text(cr->pct, pct);
        g_free(pct);

        char *rt = fmt_rate(cl->rate_now);
        char *ct = cl->ceil_bps > 0 ? fmt_rate(cl->ceil_bps) : g_strdup("—");
        char *m = g_strdup_printf("<b>%s</b> <span foreground='#78909c'>/ %s</span>", rt, ct);
        gtk_label_set_markup(cr->rate, m);
        g_free(rt); g_free(ct); g_free(m);

        const char *backlog = backlog_for_class(ifc, cl);
        if (!backlog) backlog = cl->backlog ? cl->backlog : "—";
        char *m1 = g_strdup_printf(
            "<span foreground='#ffffff'>Дропы:</span> <span foreground='#ff5252'>%llu</span>",
            (unsigned long long) cl->dropped);
        char *m2 = g_strdup_printf(
            "<span foreground='#ffffff'>Оверлимиты:</span> <span foreground='#ffb74d'>%llu</span>",
            (unsigned long long) cl->overlimits);
        char *m3 = g_strdup_printf(
            "<span foreground='#ffffff'>Бэклог:</span> <span foreground='#4fc3f7'>%s</span>", backlog);
        gtk_label_set_markup(cr->st_drops, m1);
        gtk_label_set_markup(cr->st_over, m2);
        gtk_label_set_markup(cr->st_back, m3);
        g_free(m1); g_free(m2); g_free(m3);
    }
}

static void rebuild_cards(Snapshot *s)
{
    cards_clear();

    char *dev = NULL, *ifb = NULL;
    gboolean is_ing = FALSE;
    parse_key(APP.sel_key, &dev, &ifb, &is_ing);
    const char *dn = is_ing ? ifb : dev;

    TcIface *ifc = (s && dn) ? snapshot_find_iface(s, dn) : NULL;
    if (!ifc || ifc->classes->len == 0) {
        char *m = g_strdup_printf("<i>%s</i>", dn
                ? _("Нет классов HTB на этом узле")
                : _("Выберите устройство в дереве слева"));
        gtk_label_set_markup(APP.info, m);
        g_free(m);
        g_free(dev); g_free(ifb);
        return;
    }

    char *sum = fmt_rate(iface_classes_rate(ifc));
    char *info = g_strdup_printf(
        "Иерархия: <b>%s</b> · классов: %u · сумма: <b>%s</b>",
        dn, (guint) ifc->classes->len, sum);
    gtk_label_set_markup(APP.info, info);
    g_free(sum); g_free(info);

    for (guint i = 0; i < ifc->classes->len; i++) {
        TcClass *cl = g_ptr_array_index(ifc->classes, i);
        GtkWidget *card = make_card(dn, cl, i);
        gtk_box_pack_start(GTK_BOX(APP.cards_box), card, FALSE, FALSE, 0);
    }
    APP.cards_dev = g_strdup(dn);
    APP.cards_count = ifc->classes->len;

    update_cards(s);
    gtk_widget_show_all(APP.cards_box);
    g_free(dev); g_free(ifb);
}

/* --- применение снапшота (GUI-поток) --- */

/* обновить COL_RATE одной строки; для паков (@…) — агрегат, история не пишется */
static void update_row_rate(App *a, Snapshot *s, GtkTreeIter *it)
{
    GtkTreeModel *m = GTK_TREE_MODEL(a->store);
    char *k = NULL;
    gtk_tree_model_get(m, it, COL_KEY, &k, -1);
    if (k) {
        double r = rate_for_key(s, k);
        char *rt = fmt_rate(r);
        gtk_tree_store_set(a->store, it, COL_RATE, rt, -1);
        if (k[0] != '@') hist_push(k, r);
        g_free(rt);
    }
    g_free(k);
}

static void tray_update_status(Snapshot *s);
static void on_menu_pause(GtkCheckMenuItem *mi, gpointer data);
static void apply_snapshot(App *a, Snapshot *s);

static gboolean apply_idle(gpointer data)
{
    Snapshot *s = data;
    if (g_atomic_int_get(&APP.worker_run) && APP.win)
        apply_snapshot(&APP, s);
    else
        snapshot_free(s);
    return G_SOURCE_REMOVE;
}

static void apply_snapshot(App *a, Snapshot *s)
{
    snapshot_free(a->cur);
    a->cur = s;

    /* --- 1. актуализация дерева: паки → устройства → направления --- */
    GHashTable *present = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    gboolean created_any = FALSE;

    GPtrArray *packs[PACK_N];
    for (int pi = 0; pi < PACK_N; pi++) packs[pi] = g_ptr_array_new();
    for (guint i = 0; i < s->top->len; i++) {
        TcIface *ifc = g_ptr_array_index(s->top, i);
        g_ptr_array_add(packs[classify_iface(ifc)], ifc);
    }

    for (int pi = 0; pi < PACK_N; pi++) {
        if (packs[pi]->len == 0) continue;      /* пустой пак не показываем */

        GtkTreeIter pack_it;
        gboolean created = FALSE;
        upsert_row(PACK_KEY[pi], _(PACK_TITLE[pi]), "", NULL, &pack_it, &created);
        g_hash_table_add(present, g_strdup(PACK_KEY[pi]));
        created_any |= created;

        for (guint d = 0; d < packs[pi]->len; d++) {
            TcIface *ifc = g_ptr_array_index(packs[pi], d);
            GtkTreeIter dev_it;
            gboolean created_d = FALSE;

            upsert_row(ifc->name, ifc->name,
                       ifc->is_ifb ? "<span foreground='#6a1b9a'>ifb</span>"
                                   : "<span foreground='#888888'>iface</span>",
                       &pack_it, &dev_it, &created_d);
            g_hash_table_add(present, g_strdup(ifc->name));
            created_any |= created_d;

            if (ifc->classes->len > 0 || ifc->has_htb) {
                char *ck = g_strdup_printf("%s|egress", ifc->name);
                GtkTreeIter ci;
                upsert_row(ck, "Исходящий (egress)",
                           "<span foreground='#2e7d32'>▲ egress</span>", &dev_it, &ci, &created_d);
                g_hash_table_add(present, g_strdup(ck));
                created_any |= created_d;
                g_free(ck);
            }
            for (guint k = 0; k < ifc->paths->len; k++) {
                TcIngressPath *p = g_ptr_array_index(ifc->paths, k);
                char *ck = g_strdup_printf("%s|ing|%s", ifc->name, p->ifb);
                char *nm = ingress_child_label(p, ifc->paths->len);
                GtkTreeIter ci;
                upsert_row(ck, nm, "<span foreground='#1565c0'>▼ ingress</span>", &dev_it, &ci, &created_d);
                g_free(nm);
                g_hash_table_add(present, g_strdup(ck));
                created_any |= created_d;
                g_free(ck);
            }
            remove_stale(&dev_it, present);
        }
        remove_stale(&pack_it, present);
        g_ptr_array_unref(packs[pi]);
    }
    remove_stale(NULL, present);
    if (created_any)
        gtk_tree_view_expand_all(GTK_TREE_VIEW(a->tree));

    /* --- 2. колонка скорости + история (пак → устройство → направление) --- */
    GtkTreeModel *m = GTK_TREE_MODEL(a->store);
    GtkTreeIter it;
    gboolean ok = gtk_tree_model_get_iter_first(m, &it);
    while (ok) {
        update_row_rate(a, s, &it);
        GtkTreeIter dev_it;
        gboolean okd = gtk_tree_model_iter_children(m, &dev_it, &it);
        while (okd) {
            update_row_rate(a, s, &dev_it);
            GtkTreeIter ch;
            gboolean okc = gtk_tree_model_iter_children(m, &ch, &dev_it);
            while (okc) {
                update_row_rate(a, s, &ch);
                okc = gtk_tree_model_iter_next(m, &ch);
            }
            okd = gtk_tree_model_iter_next(m, &dev_it);
        }
        ok = gtk_tree_model_iter_next(m, &it);
    }

    /* --- 2b. истории по классам и сумме ingress (для стека графика) --- */
    for (guint i = 0; i < s->ifaces->len; i++) {
        TcIface *ifc = g_ptr_array_index(s->ifaces, i);
        for (guint k = 0; k < ifc->classes->len; k++) {
            TcClass *cl = g_ptr_array_index(ifc->classes, k);
            char *hk = g_strdup_printf("cls|%s|%s", ifc->name, cl->classid);
            hist_push(hk, cl->rate_now);
            g_free(hk);
        }
        if (ifc->paths->len > 0) {
            char *hk = g_strdup_printf("ingsum|%s", ifc->name);
            hist_push(hk, iface_paths_rate(ifc));
            g_free(hk);
        }
    }

    g_hash_table_unref(present);

    /* --- 3. выбор по умолчанию: первое выбираемое устройство (не пак) --- */
    if (!a->sel_key && gtk_tree_model_get_iter_first(m, &it)) {
        GtkTreeIter found;
        gboolean have = FALSE;
        GtkTreeIter dev_it;
        gboolean okd = gtk_tree_model_iter_children(m, &dev_it, &it);
        while (okd && !have) {
            char *kd = NULL;
            gtk_tree_model_get(m, &dev_it, COL_KEY, &kd, -1);
            if (kd && kd[0] != '@') { found = dev_it; have = TRUE; }
            g_free(kd);
            if (!have) okd = gtk_tree_model_iter_next(m, &dev_it);
        }
        if (have) {
            char *k = NULL;
            gtk_tree_model_get(m, &found, COL_KEY, &k, -1);
            a->sel_key = g_strdup(k);
            g_free(k);
            gtk_tree_selection_select_iter(
                gtk_tree_view_get_selection(GTK_TREE_VIEW(a->tree)), &found);
        }
    }

    /* --- 4. карточки: пересборка при смене узла/состава, иначе обновление --- */
    char *want_dev = NULL, *want_ifb = NULL;
    gboolean want_ing = FALSE;
    parse_key(a->sel_key, &want_dev, &want_ifb, &want_ing);
    const char *dn = want_ing ? want_ifb : want_dev;
    TcIface *sel_ifc = dn ? snapshot_find_iface(s, dn) : NULL;
    if (!dn || !sel_ifc || sel_ifc->classes->len != a->cards_count ||
        (a->cards_dev && strcmp(dn, a->cards_dev) != 0) || !a->cards_dev)
        rebuild_cards(s);
    else
        update_cards(s);
    g_free(want_dev); g_free(want_ifb);

    /* --- 5. инфострока --- */
    if (a->sel_key) {
        double r = rate_for_key(s, a->sel_key);
        char *rt = fmt_rate(r);
        char *m = g_strdup_printf("Узел: <b>%s</b> · скорость: <b>%s</b>", a->sel_key, rt);
        gtk_label_set_markup(a->info, m);
        g_free(m); g_free(rt);
    }

    /* --- 6. статус --- */
    {
        GDateTime *dt = g_date_time_new_now_local();
        char *ts = g_date_time_format(dt, "%H:%M:%S");
        char *st;
        if (s->mock)
            st = g_strdup_printf("<span size='small' foreground='#e65100'>ДЕМО: tc не найден — показаны мок-данные · период %d с · обновлено %s</span>",
                                 (int)(g_atomic_int_get(&a->interval_ms) / 1000), ts);
        else if (s->error)
            st = g_strdup_printf("<span size='small' foreground='#c62828'>ошибка: %s · период %d с</span>", s->error,
                                 (int)(g_atomic_int_get(&a->interval_ms) / 1000));
        else
            st = g_strdup_printf("<span size='small' foreground='#607d8b'>обновлено %s · период %d с · tc: %s</span>",
                                 ts, (int)(g_atomic_int_get(&a->interval_ms) / 1000), a->tc_path);
        gtk_label_set_markup(a->status, st);
        gtk_header_bar_set_subtitle(GTK_HEADER_BAR(a->header),
                                    s->mock ? "ДЕМО-режим" : g_get_host_name());
        tray_update_status(s);
        g_free(st); g_free(ts); g_date_time_unref(dt);
    }

    gtk_widget_queue_draw(a->graph);
}

/* --- график: 60-секундная история выбранного узла --- */

static double nice_ceil(double x)
{
    if (x <= 0) return 1e6;
    double e = pow(10.0, floor(log10(x)));
    double m = x / e;
    return (m <= 1 ? 1 : m <= 2 ? 2 : m <= 5 ? 5 : 10) * e;
}

static gboolean on_graph_draw(GtkWidget *w, cairo_t *cr, gpointer data)
{
    (void) data;
    GtkAllocation al;
    gtk_widget_get_allocation(w, &al);
    double W = al.width, H = al.height;

    /* фон */
    cairo_set_source_rgb(cr, 0.075, 0.082, 0.10);
    cairo_rectangle(cr, 0, 0, W, H);
    cairo_fill(cr);

    const char *key = APP.sel_key;
    History *h = hist_get(key);
    gboolean is_pack = (key && key[0] == '@');
    gboolean is_ing  = (key && strstr(key, "|ing|"));
    double step = W / (double)(HISTORY_LEN - 1);

    char *dev = NULL, *ifb = NULL;
    gboolean ing_flag = FALSE;
    parse_key(key, &dev, &ifb, &ing_flag);
    const char *dev_name = is_ing ? ifb : dev;
    TcIface *ifc = APP.cur ? snapshot_find_iface(APP.cur, dev_name) : NULL;

    /* ---------- пак: линия агрегата (стек по устройствам — позже) ---------- */
    if (is_pack) {
        double scale = (h && h->rmax > 0) ? nice_ceil(h->rmax * 1.05) : 1e6;
        cairo_set_source_rgb(cr, 0.17, 0.19, 0.22);
        cairo_set_line_width(cr, 1.0);
        for (int g = 1; g <= 3; g++) {
            double y = H * g / 4.0;
            cairo_move_to(cr, 0, y); cairo_line_to(cr, W, y); cairo_stroke(cr);
        }
        if (h && h->len >= 1) {
            cairo_set_source_rgb(cr, 0.57, 0.64, 0.71);
            cairo_set_line_width(cr, 1.6);
            for (int i = 0; i < h->len; i++) {
                double x = W - (h->len - 1 - i) * step;
                double y = H - 4 - (h->v[i] / scale) * (H - 22);
                if (i == 0) cairo_move_to(cr, x, y);
                else cairo_line_to(cr, x, y);
            }
            cairo_stroke(cr);
        }
        char *cur = fmt_rate(h && h->len ? h->v[h->len - 1] : 0);
        PangoLayout *pl = gtk_widget_create_pango_layout(w, NULL);
        char *m = g_strdup_printf("<span foreground='#cfd8dc' size='small'><b>%s</b></span>", cur);
        pango_layout_set_markup(pl, m, -1);
        cairo_move_to(cr, 8, 4);
        pango_cairo_show_layout(cr, pl);
        g_object_unref(pl);
        g_free(m); g_free(cur);
        g_free(dev); g_free(ifb);
        return FALSE;
    }

    /* ---------- устройство/узел: стек классов в порядке очередей ---------- */

    /* прямые дети корня — они и есть полосы стека */
    guint nstk = 0;
    guint idx[HISTORY_LEN];
    if (ifc)
        for (guint k = 0; k < ifc->classes->len && nstk < HISTORY_LEN; k++) {
            TcClass *cl = g_ptr_array_index(ifc->classes, k);
            if (cl->depth == 1) idx[nstk++] = k;
        }

    /* Σ прямых детей (для подписи и дельты) */
    double dsum = 0;
    for (guint k = 0; k < nstk; k++) {
        TcClass *cl = g_ptr_array_index(ifc->classes, idx[k]);
        dsum += cl->rate_now;
    }
    TcClass *rc = root_class_of(ifc);
    double delta = rc ? rc->rate_now - dsum : 0.0;

    /* шкала: rate корня → линк → аплинк → счётчики */
    char *src = NULL;
    double scale = scale_for_node(APP.cur, key, &src);
    if (scale <= 0) scale = (h && h->rmax > 0) ? h->rmax : 1e6;

    gboolean is_egr = (key && strstr(key, "|egress"));
    gboolean has_ing = (!is_ing && !is_egr && ifc && ifc->paths->len > 0);
    double pad_t = 20, pad_b = 4, gap = 3;
    double main_h, ing_h;
    if (has_ing) {
        main_h = (H - pad_t - pad_b - gap) * 0.72;
        ing_h  = (H - pad_t - pad_b - gap) * 0.26;
    } else {
        main_h = H - pad_t - pad_b;
        ing_h  = 0;
    }
    double base_y = pad_t + main_h;

    /* сетка основной секции */
    cairo_set_source_rgb(cr, 0.17, 0.19, 0.22);
    cairo_set_line_width(cr, 1.0);
    for (int g = 1; g <= 3; g++) {
        double y = pad_t + main_h * g / 4.0;
        cairo_move_to(cr, 0, y);
        cairo_line_to(cr, W, y);
        cairo_stroke(cr);
    }

    /* стек: кумулятивные полосы по слотам кольца (сначала только данные) */
    double *lo = g_new0(double, (gsize)(nstk ? nstk : 1) * HISTORY_LEN);
    double *hi = g_new0(double, (gsize)(nstk ? nstk : 1) * HISTORY_LEN);
    double *run = g_new0(double, HISTORY_LEN);

    for (guint kk = 0; kk < nstk; kk++) {
        TcClass *cl = g_ptr_array_index(ifc->classes, idx[kk]);
        char *hk = g_strdup_printf("cls|%s|%s", dev_name, cl->classid);
        History *hc = hist_get(hk);
        g_free(hk);
        for (int s2 = 0; s2 < HISTORY_LEN; s2++) {
            lo[kk * HISTORY_LEN + s2] = run[s2];
            run[s2] += hist_value_at(hc, s2);
            hi[kk * HISTORY_LEN + s2] = run[s2];
        }
    }

    /* авто-зум: пик окна < 25% предела — растягиваем шкалу на всю высоту */
    double wmax = 0;
    for (int s2 = 0; s2 < HISTORY_LEN; s2++)
        if (run[s2] > wmax) wmax = run[s2];
    double zoom = 1.0;
    if (scale > 0 && wmax > 0 && wmax < 0.25 * scale) {
        double target = nice_ceil(wmax * 1.2);
        if (target > 0 && target < scale) {
            zoom = scale / target;
            scale = target;
        }
    }

    /* отрисовка полос по подготовленным кумулятивам */
    for (guint kk = 0; kk < nstk; kk++) {
        TcClass *cl = g_ptr_array_index(ifc->classes, idx[kk]);
        const Rgb *col = pal_color(kk);

        /* верхняя кромка суммарного стека (только у первой очереди) */
        if (kk == 0) {
            cairo_set_source_rgba(cr, 0.72, 0.77, 0.80, 0.55);
            cairo_set_line_width(cr, 1.0);
            for (int s2 = 0; s2 < HISTORY_LEN; s2++) {
                double x = W - (HISTORY_LEN - 1 - s2) * step;
                double y = base_y - (run[s2] / scale) * main_h;
                if (y < pad_t) y = pad_t;
                if (s2 == 0) cairo_move_to(cr, x, y);
                else cairo_line_to(cr, x, y);
            }
            cairo_stroke(cr);
        }

        /* полоса очереди: от границы предыдущей до своей */
        cairo_set_source_rgba(cr, col->r, col->g, col->b, 0.38);
        for (int s2 = 0; s2 < HISTORY_LEN; s2++) {
            double x = W - (HISTORY_LEN - 1 - s2) * step;
            double y_top = base_y - (hi[kk * HISTORY_LEN + s2] / scale) * main_h;
            if (y_top < pad_t) y_top = pad_t;
            if (s2 == 0) cairo_move_to(cr, x, y_top);
            else cairo_line_to(cr, x, y_top);
        }
        for (int s2 = HISTORY_LEN - 1; s2 >= 0; s2--) {
            double x = W - (HISTORY_LEN - 1 - s2) * step;
            double y_lo = base_y - (lo[kk * HISTORY_LEN + s2] / scale) * main_h;
            cairo_line_to(cr, x, y_lo);
        }
        cairo_close_path(cr);
        cairo_fill(cr);

        /* подпись classid внутри полосы */
        double hpx = (hi[kk * HISTORY_LEN + HISTORY_LEN - 1] -
                      lo[kk * HISTORY_LEN + HISTORY_LEN - 1]) / scale * main_h;
        if (hpx > 11) {
            PangoLayout *pl = gtk_widget_create_pango_layout(w, NULL);
            char *m = g_strdup_printf("<span foreground='#eceff1' size='small'>%s</span>", cl->classid);
            pango_layout_set_markup(pl, m, -1);
            int pw = 0, ph2 = 0;
            pango_layout_get_pixel_size(pl, &pw, &ph2);
            double mid = (hi[kk * HISTORY_LEN + HISTORY_LEN - 1] +
                          lo[kk * HISTORY_LEN + HISTORY_LEN - 1]) / 2.0;
            double my = base_y - (mid / scale) * main_h - ph2 / 2.0;
            cairo_move_to(cr, W - pw - 6, my);
            pango_cairo_show_layout(cr, pl);
            g_object_unref(pl);
            g_free(m);
        }
    }
    g_free(lo); g_free(hi); g_free(run);

    /* ---------- ingress: серая полоса в отдельной нижней секции ---------- */
    if (has_ing) {
        double iy = pad_t + main_h + gap;
        cairo_set_source_rgb(cr, 0.20, 0.22, 0.26);
        cairo_move_to(cr, 0, iy - 1.5);
        cairo_line_to(cr, W, iy - 1.5);
        cairo_stroke(cr);

        char *hk = g_strdup_printf("ingsum|%s", dev_name);
        History *hs = hist_get(hk);
        g_free(hk);
        if (hs && hs->len >= 1) {
            cairo_set_source_rgba(cr, 0.62, 0.66, 0.70, 0.38);
            for (int s2 = 0; s2 < HISTORY_LEN; s2++) {
                double v = hist_value_at(hs, s2);
                double x = W - (HISTORY_LEN - 1 - s2) * step;
                double y = iy + ing_h - (v / scale) * ing_h;
                if (s2 == 0) cairo_move_to(cr, x, y);
                else cairo_line_to(cr, x, y);
            }
            cairo_line_to(cr, W, iy + ing_h);
            cairo_line_to(cr, W - (HISTORY_LEN - 1) * step, iy + ing_h);
            cairo_close_path(cr);
            cairo_fill(cr);
        }
        PangoLayout *pl = gtk_widget_create_pango_layout(w, NULL);
        char *im = g_strdup_printf("<span foreground='#90a4ae' size='small'>ingress ↓ <b>%s</b></span>",
                                   fmt_rate(iface_paths_rate(ifc)));
        pango_layout_set_markup(pl, im, -1);
        cairo_move_to(cr, 4, iy + 2);
        pango_cairo_show_layout(cr, pl);
        g_object_unref(pl);
        g_free(im);
    }

    /* ---------- подписи: шкала · коэффициент зума · сейчас · дельта ---------- */
    char *lbl1;
    if (zoom >= 1.05) {
        char *zs = (zoom >= 10) ? g_strdup_printf("×%.0f", zoom)
                                : g_strdup_printf("×%.1f", zoom);
        lbl1 = g_strdup_printf(
            "<span foreground='#cfd8dc' size='small'>шкала: <b>%s</b> · %s · <span foreground='#ffb300'>%s</span></span>",
            fmt_rate(scale), src ? src : "?", zs);
        g_free(zs);
    } else {
        lbl1 = g_strdup_printf(
            "<span foreground='#cfd8dc' size='small'>шкала: <b>%s</b> · %s</span>",
            fmt_rate(scale), src ? src : "?");
    }
    char *lbl2 = g_strdup_printf(
        "<span foreground='#cfd8dc' size='small'>сейчас: <b>%s</b></span>", fmt_rate(dsum));
    char *dcol = (fabs(delta) > scale * 0.03) ? "#ffb300" : "#78909c";
    char *lbl3 = g_strdup_printf(
        "<span foreground='%s' size='small'>Δ корень−Σ: %s</span>", dcol, fmt_rate(delta));

    PangoLayout *pl = gtk_widget_create_pango_layout(w, NULL);
    pango_layout_set_markup(pl, lbl1, -1);
    cairo_move_to(cr, 8, 3);
    pango_cairo_show_layout(cr, pl);
    g_object_unref(pl);

    pl = gtk_widget_create_pango_layout(w, NULL);
    pango_layout_set_markup(pl, lbl2, -1);
    int pw = 0, ph = 0;
    pango_layout_get_pixel_size(pl, &pw, &ph);
    cairo_move_to(cr, W - pw - 8, 3);
    pango_cairo_show_layout(cr, pl);
    g_object_unref(pl);

    pl = gtk_widget_create_pango_layout(w, NULL);
    pango_layout_set_markup(pl, lbl3, -1);
    pango_layout_get_pixel_size(pl, &pw, &ph);
    cairo_move_to(cr, W - pw - 8, 20);
    pango_cairo_show_layout(cr, pl);
    g_object_unref(pl);

    g_free(lbl1); g_free(lbl2); g_free(lbl3);
    g_free(src);
    g_free(dev); g_free(ifb);
    return FALSE;
}

/* =========================== СИГНАЛЫ =========================== */

static void on_sel_changed(GtkTreeSelection *sel, gpointer data)
{
    (void) data;
    GtkTreeIter it;
    char *k = NULL;
    if (gtk_tree_selection_get_selected(sel, NULL, &it))
        gtk_tree_model_get(GTK_TREE_MODEL(APP.store), &it, COL_KEY, &k, -1);
    if (k && k[0] == '@') { g_free(k); k = NULL; }   /* паки — не выбираются */
    g_free(APP.sel_key);
    APP.sel_key = k;
    rebuild_cards(APP.cur);
    gtk_widget_queue_draw(APP.graph);
}

static void on_pause_toggled(GtkToggleButton *b, gpointer data)
{
    App *a = data;
    gboolean act = gtk_toggle_button_get_active(b);
    g_atomic_int_set(&a->paused, act ? 1 : 0);
    if (a->mi_pause) {  /* синхронизация галки в меню трея */
        g_signal_handlers_block_by_func(a->mi_pause, on_menu_pause, NULL);
        gtk_check_menu_item_set_active(a->mi_pause, act);
        g_signal_handlers_unblock_by_func(a->mi_pause, on_menu_pause, NULL);
    }
}

static void on_interval_changed(GtkSpinButton *b, gpointer data)
{
    App *a = data;
    g_atomic_int_set(&a->interval_ms, (gint)(gtk_spin_button_get_value(b) * 1000.0));
}

/* «Во всю высоту»: прячем верхнюю панель — график занимает всё окно */
static void on_expand_toggled(GtkToggleButton *b, App *a)
{
    gboolean act = gtk_toggle_button_get_active(b);
    gtk_widget_set_visible(a->hp, !act);
}

/* =========================== РАБОЧИЙ ПОТОК =========================== */

static gpointer worker_loop(gpointer data)
{
    App *a = data;
    Collector *c = collector_new();
    g_free(c->tc); c->tc = g_strdup(a->tc_path);
    g_free(c->ip); c->ip = g_strdup(a->ip_path);

    while (g_atomic_int_get(&a->worker_run)) {
        if (!g_atomic_int_get(&a->paused)) {
            Snapshot *s = collect_snapshot(c);
            g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, apply_idle, s, NULL);
        }
        /* спим интервал порциями по 50 мс — быстро реагируем на стоп/паузу */
        int left = (int) g_atomic_int_get(&a->interval_ms);
        while (left > 0 && g_atomic_int_get(&a->worker_run)) {
            g_usleep(50000);
            left -= 50;
        }
    }
    collector_free(c);
    return NULL;
}

/* ============ STRIPED-РЕНДЕРЕР: заголовки паков (аналог conky striped_hr) ============ */

#define TC_TYPE_STRIPED (tc_striped_get_type())
typedef struct _TcStriped TcStriped;
typedef struct _TcStripedClass TcStripedClass;

struct _TcStriped { GtkCellRenderer parent; char *label; };
struct _TcStripedClass { GtkCellRendererClass parent_class; };

enum { SP_PROP_0, SP_PROP_LABEL, SP_PROP_N };

G_DEFINE_TYPE(TcStriped, tc_striped, GTK_TYPE_CELL_RENDERER)

static GtkCellRenderer *tc_striped_new(void)
{
    return GTK_CELL_RENDERER(g_object_new(TC_TYPE_STRIPED, NULL));
}

static void tc_striped_get_property(GObject *o, guint id, GValue *v, GParamSpec *ps)
{
    TcStriped *r = (TcStriped *) o;
    if (id == SP_PROP_LABEL) g_value_set_string(v, r->label ? r->label : "");
    else G_OBJECT_WARN_INVALID_PROPERTY_ID(o, id, ps);
}

static void tc_striped_set_property(GObject *o, guint id, const GValue *v, GParamSpec *ps)
{
    TcStriped *r = (TcStriped *) o;
    if (id == SP_PROP_LABEL) {
        g_free(r->label);
        r->label = g_value_dup_string(v);
    } else G_OBJECT_WARN_INVALID_PROPERTY_ID(o, id, ps);
}

static void tc_striped_finalize(GObject *o)
{
    TcStriped *r = (TcStriped *) o;
    g_free(r->label);
    G_OBJECT_CLASS(tc_striped_parent_class)->finalize(o);
}

static void tc_striped_get_size(GtkCellRenderer *cell, GtkWidget *widget,
                                const GdkRectangle *cell_area,
                                gint *xoff, gint *yoff, gint *width, gint *height)
{
    (void) cell_area; (void) xoff; (void) yoff;
    TcStriped *r = (TcStriped *) cell;
    PangoLayout *pl = gtk_widget_create_pango_layout(widget, NULL);
    char *mk = g_strdup_printf("<span size='small' weight='bold'>%s</span>",
                               r->label ? r->label : "");
    pango_layout_set_markup(pl, mk, -1);
    int tw = 0, th = 0;
    pango_layout_get_pixel_size(pl, &tw, &th);
    if (width)  *width  = tw + 14;
    if (height) *height = (th > 14 ? th : 14) + 6;
    g_object_unref(pl);
    g_free(mk);
}

static void tc_striped_render(GtkCellRenderer *cell, cairo_t *cr, GtkWidget *widget,
                              const GdkRectangle *bg, const GdkRectangle *cell_area,
                              GtkCellRendererState flags)
{
    (void) bg; (void) flags; (void) widget;
    TcStriped *r = (TcStriped *) cell;

    const double band_h = 5.0;
    double ty = cell_area->y + (cell_area->height - band_h) / 2.0;

    /* заголовок пака слева */
    PangoLayout *pl = gtk_widget_create_pango_layout(widget, NULL);
    char *mk = g_strdup_printf("<span size='small' weight='bold' foreground='#cfd8dc'>%s</span>",
                               r->label ? r->label : "");
    pango_layout_set_markup(pl, mk, -1);
    int tw = 0, th = 0;
    pango_layout_get_pixel_size(pl, &tw, &th);
    cairo_move_to(cr, cell_area->x + 2, cell_area->y + (cell_area->height - th) / 2.0);
    pango_cairo_show_layout(cr, pl);
    g_object_unref(pl);
    g_free(mk);

    /* диагональная штриховка от конца заголовка до правого края строки */
    double sx = cell_area->x + tw + 10;
    double ex = cell_area->x + cell_area->width - 2;
    if (ex - sx > 4) {
        cairo_save(cr);
        cairo_rectangle(cr, sx, ty, ex - sx, band_h);
        cairo_clip(cr);
        cairo_set_line_width(cr, 1.4);
        for (double x = sx - band_h; x < ex + band_h; x += 4.0) {
            cairo_set_source_rgba(cr, 0.565, 0.643, 0.686, 0.50);  /* #90a4ae */
            cairo_move_to(cr, x, ty + band_h);
            cairo_line_to(cr, x + band_h, ty);
            cairo_stroke(cr);
            cairo_set_source_rgba(cr, 0.216, 0.278, 0.310, 0.50);  /* #37474f */
            cairo_move_to(cr, x + 2, ty + band_h);
            cairo_line_to(cr, x + 2 + band_h, ty);
            cairo_stroke(cr);
        }
        cairo_restore(cr);
    }
}

static void tc_striped_init(TcStriped *r)
{
    (void) r;
}

static void tc_striped_class_init(TcStripedClass *klass)
{
    GObjectClass *goc = G_OBJECT_CLASS(klass);
    GtkCellRendererClass *crc = GTK_CELL_RENDERER_CLASS(klass);
    goc->get_property = tc_striped_get_property;
    goc->set_property = tc_striped_set_property;
    goc->finalize     = tc_striped_finalize;
    crc->get_size     = tc_striped_get_size;
    crc->render       = tc_striped_render;
    g_object_class_install_property(goc, SP_PROP_LABEL,
        g_param_spec_string("label", "Label", "Pack title", "",
                            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
}

/* видимость рендереров колонки имени: text — для устройств/узлов, striped — для паков */
static void pack_vis_func(GtkTreeViewColumn *col, GtkCellRenderer *cell,
                          GtkTreeModel *m, GtkTreeIter *it, gpointer data)
{
    (void) col;
    gboolean want = GPOINTER_TO_INT(data);
    char *k = NULL;
    gtk_tree_model_get(m, it, COL_KEY, &k, -1);
    gboolean is_pack = (k && k[0] == '@');
    g_free(k);
    g_object_set(cell, "visible", is_pack == want, NULL);
}

static void striped_data_func(GtkTreeViewColumn *col, GtkCellRenderer *cell,
                              GtkTreeModel *m, GtkTreeIter *it, gpointer data)
{
    (void) col; (void) data;
    char *k = NULL, *nm = NULL;
    gtk_tree_model_get(m, it, COL_KEY, &k, COL_NAME, &nm, -1);
    gboolean is_pack = (k && k[0] == '@');
    g_object_set(cell, "visible", is_pack, "label", nm ? nm : "", NULL);
    g_free(k); g_free(nm);
}

/* паки — только разделители: выделение запрещено */
static gboolean sel_ok(GtkTreeSelection *sel, GtkTreeModel *m, GtkTreePath *path,
                       gboolean cur, gpointer data)
{
    (void) sel; (void) cur; (void) data;
    GtkTreeIter it;
    char *k = NULL;
    gboolean allow = TRUE;
    if (gtk_tree_model_get_iter(m, &it, path)) {
        gtk_tree_model_get(m, &it, COL_KEY, &k, -1);
        allow = !(k && k[0] == '@');
        g_free(k);
    }
    return allow;
}

/* =========================== ТРЕЙ =========================== */
/* Крестик и «свернуть» прячут окно в трей (окно не разрушается).
 * Иконка: libappindicator (StatusNotifier) или GtkStatusIcon (XEmbed).
 * Полный выход — только пункт «Выход» в меню трея. */

static void tray_toggle_window(void)
{
    if (!APP.win) return;
    if (gtk_widget_get_visible(APP.win)) {
        gtk_widget_hide(APP.win);
    } else {
        gtk_window_deiconify(GTK_WINDOW(APP.win));
        gtk_widget_show(APP.win);
        gtk_window_present(GTK_WINDOW(APP.win));
    }
}

static void on_menu_show(GtkMenuItem *mi, gpointer data)
{
    (void) mi; (void) data;
    tray_toggle_window();
}

static void on_menu_pause(GtkCheckMenuItem *mi, gpointer data)
{
    (void) data;
    gboolean act = gtk_check_menu_item_get_active(mi);
    if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(APP.btn_pause)) != act)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(APP.btn_pause), act);
}

static void on_menu_quit(GtkMenuItem *mi, gpointer data)
{
    (void) mi; (void) data;
    g_application_quit(G_APPLICATION(APP.app));
}

static gboolean win_delete_event(GtkWidget *w, GdkEvent *e, gpointer data)
{
    (void) w; (void) e; (void) data;
    gtk_widget_hide(APP.win);
    return TRUE;
}

static gboolean win_state_event(GtkWidget *w, GdkEventWindowState *e, gpointer data)
{
    (void) w; (void) data;
    if ((e->changed_mask & GDK_WINDOW_STATE_ICONIFIED) &&
        (e->new_window_state & GDK_WINDOW_STATE_ICONIFIED))
        gtk_widget_hide(APP.win);
    return FALSE;
}

static GtkWidget *tray_build_menu(void)
{
    GtkWidget *menu = gtk_menu_new();
    GtkWidget *mi_show = gtk_menu_item_new_with_label(_("Показать / скрыть"));
    APP.mi_pause = GTK_CHECK_MENU_ITEM(gtk_check_menu_item_new_with_label(_("Пауза опроса")));
    GtkWidget *sep = gtk_separator_menu_item_new();
    GtkWidget *mi_quit = gtk_menu_item_new_with_label(_("Выход"));

    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi_show);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), GTK_WIDGET(APP.mi_pause));
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), sep);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi_quit);

    g_signal_connect(mi_show, "activate", G_CALLBACK(on_menu_show), NULL);
    g_signal_connect(APP.mi_pause, "toggled", G_CALLBACK(on_menu_pause), NULL);
    g_signal_connect(mi_quit, "activate", G_CALLBACK(on_menu_quit), NULL);

    gtk_widget_show_all(menu);
    return menu;
}

#ifdef HAVE_APPINDICATOR

static void tray_init(void)
{
    APP.tray_menu = tray_build_menu();
    APP.tray = app_indicator_new("shaping-view", "network-wired",
                                 APP_INDICATOR_CATEGORY_APPLICATION_STATUS);
    app_indicator_set_status(APP.tray, APP_INDICATOR_STATUS_ACTIVE);
    app_indicator_set_icon_full(APP.tray, "network-wired", "shaping-view");
    app_indicator_set_menu(APP.tray, GTK_MENU(APP.tray_menu));
    app_indicator_set_title(APP.tray, "Шейпинг tc");
}

static void tray_update_status(Snapshot *s)
{
    if (!APP.tray || !s) return;
    double r = 0;
    for (guint i = 0; i < s->top->len; i++) {
        TcIface *ifc = g_ptr_array_index(s->top, i);
        if (!ifc->is_ifb) r += iface_classes_rate(ifc) + iface_paths_rate(ifc);
    }
    char *rt = fmt_rate(r);
    char *t = g_strdup_printf("Шейпинг tc · суммарно: %s%s", rt, s->mock ? " (ДЕМО)" : "");
    app_indicator_set_title(APP.tray, t);
    g_free(rt); g_free(t);
}

#else  /* GtkStatusIcon (XEmbed) — deprecated, но работает в XEmbed-треях */

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"

static void tray_activate_cb(GtkStatusIcon *icon, gpointer data)
{
    (void) icon; (void) data;
    tray_toggle_window();
}

static void tray_popup_cb(GtkStatusIcon *icon, guint button, guint at, gpointer data)
{
    (void) icon; (void) data;
    gtk_menu_popup(GTK_MENU(APP.tray_menu), NULL, NULL,
                   gtk_status_icon_position_menu, APP.tray, button, at);
}

static void tray_init(void)
{
    APP.tray_menu = tray_build_menu();
    APP.tray = gtk_status_icon_new_from_icon_name("network-wired");
    gtk_status_icon_set_tooltip_markup(APP.tray, "Шейпинг tc");
    gtk_status_icon_set_visible(APP.tray, TRUE);
    g_signal_connect(APP.tray, "activate", G_CALLBACK(tray_activate_cb), NULL);
    g_signal_connect(APP.tray, "popup-menu", G_CALLBACK(tray_popup_cb), NULL);
}

static void tray_update_status(Snapshot *s)
{
    if (!APP.tray || !s) return;
    double r = 0;
    for (guint i = 0; i < s->top->len; i++) {
        TcIface *ifc = g_ptr_array_index(s->top, i);
        if (!ifc->is_ifb) r += iface_classes_rate(ifc) + iface_paths_rate(ifc);
    }
    char *rt = fmt_rate(r);
    char *t = g_strdup_printf("Шейпинг tc · суммарно: %s%s", rt, s->mock ? " (ДЕМО)" : "");
    gtk_status_icon_set_tooltip_markup(APP.tray, t);
    g_free(rt); g_free(t);
}

#pragma GCC diagnostic pop
#endif

/* =========================== СБОРКА UI =========================== */

/* CSS: чёрный скруглённый фон чипов статистики в карточках */
static void load_css(void)
{
    GtkCssProvider *prov = gtk_css_provider_new();
    gtk_css_provider_load_from_data(prov,
        ".chip { background-color: rgb(10,10,12); border-radius: 7px; padding: 2px 9px; }\n"
        ".mono { font-family: monospace; }\n",
        -1, NULL);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),
        GTK_STYLE_PROVIDER(prov), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(prov);
}

static GtkWidget *make_left(void)
{
    GtkWidget *sc = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sc),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);

    APP.store = gtk_tree_store_new(N_COLS, G_TYPE_STRING, G_TYPE_STRING,
                                   G_TYPE_STRING, G_TYPE_STRING);
    APP.tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(APP.store));
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(APP.tree), TRUE);

    GtkCellRenderer *r;
    GtkTreeViewColumn *c;

    /* колонка имени: обычный текст + striped-рендерер для строк паков (@…) */
    c = gtk_tree_view_column_new();
    gtk_tree_view_column_set_title(c, _("Устройство / узел"));
    gtk_tree_view_column_set_expand(c, TRUE);

    r = gtk_cell_renderer_text_new();
    g_object_set(r, "ellipsize", PANGO_ELLIPSIZE_END, NULL);
    gtk_tree_view_column_pack_start(c, r, TRUE);
    gtk_tree_view_column_set_attributes(c, r, "text", COL_NAME, NULL);
    gtk_tree_view_column_set_cell_data_func(c, r, pack_vis_func,
                                            GINT_TO_POINTER(FALSE), NULL);

    GtkCellRenderer *sr = tc_striped_new();
    gtk_tree_view_column_pack_start(c, sr, TRUE);
    gtk_tree_view_column_set_cell_data_func(c, sr, striped_data_func, NULL, NULL);

    gtk_tree_view_append_column(GTK_TREE_VIEW(APP.tree), c);

    r = gtk_cell_renderer_text_new();
    c = gtk_tree_view_column_new_with_attributes(_("Направление"), r,
                                                 "markup", COL_BADGE, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(APP.tree), c);

    r = gtk_cell_renderer_text_new();
    g_object_set(r, "xalign", 1.0, "family", "monospace", NULL);
    c = gtk_tree_view_column_new_with_attributes(_("Скорость"), r,
                                                 "text", COL_RATE, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(APP.tree), c);

    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(APP.tree));
    gtk_tree_selection_set_mode(sel, GTK_SELECTION_BROWSE);
    gtk_tree_selection_set_select_function(sel, sel_ok, NULL, NULL);
    g_signal_connect(sel, "changed", G_CALLBACK(on_sel_changed), NULL);

    gtk_container_add(GTK_CONTAINER(sc), APP.tree);
    gtk_widget_set_size_request(sc, 300, -1);
    return sc;
}

static GtkWidget *make_right(void)
{
    GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(v), 6);

    APP.info = GTK_LABEL(gtk_label_new(_("Выберите устройство слева")));
    gtk_label_set_xalign(APP.info, 0.0);
    gtk_box_pack_start(GTK_BOX(v), GTK_WIDGET(APP.info), FALSE, FALSE, 0);

    GtkWidget *sc = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sc),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    APP.cards_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_add(GTK_CONTAINER(sc), APP.cards_box);
    gtk_box_pack_start(GTK_BOX(v), sc, TRUE, TRUE, 0);
    return v;
}

static void build_ui(App *a)
{
    load_css();
    a->win = gtk_application_window_new(a->app);
    gtk_window_set_default_size(GTK_WINDOW(a->win), 1280, 860);

    GtkWidget *hb = gtk_header_bar_new();
    gtk_header_bar_set_show_close_button(GTK_HEADER_BAR(hb), TRUE);
    gtk_header_bar_set_title(GTK_HEADER_BAR(hb), _("Шейпинг tc — обзор"));
    gtk_header_bar_set_has_subtitle(GTK_HEADER_BAR(hb), TRUE);
    gtk_header_bar_set_subtitle(GTK_HEADER_BAR(hb), g_get_host_name());
    gtk_window_set_titlebar(GTK_WINDOW(a->win), hb);
    a->header = hb;

    a->btn_pause = gtk_toggle_button_new_with_label(_("Пауза"));
    gtk_widget_set_tooltip_text(a->btn_pause, _("Приостановить/возобновить опрос"));
    g_signal_connect(a->btn_pause, "toggled", G_CALLBACK(on_pause_toggled), a);
    gtk_header_bar_pack_start(GTK_HEADER_BAR(hb), a->btn_pause);

    a->spin = gtk_spin_button_new_with_range(1, 10, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(a->spin), DEF_INTERVAL_MS / 1000);
    gtk_widget_set_tooltip_text(a->spin, _("Период опроса, с"));
    g_signal_connect(a->spin, "value-changed", G_CALLBACK(on_interval_changed), a);
    gtk_header_bar_pack_start(GTK_HEADER_BAR(hb), a->spin);

    a->btn_expand = gtk_toggle_button_new_with_label(_("Во всю высоту"));
    gtk_widget_set_tooltip_text(a->btn_expand,
                                _("Растянуть график: спрятать список очередей сверху"));
    g_signal_connect(a->btn_expand, "toggled", G_CALLBACK(on_expand_toggled), a);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(hb), a->btn_expand);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 6);

    GtkWidget *hp = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_paned_pack1(GTK_PANED(hp), make_left(), TRUE, FALSE);
    gtk_paned_pack2(GTK_PANED(hp), make_right(), TRUE, FALSE);
    gtk_paned_set_position(GTK_PANED(hp), 340);

    GtkWidget *gframe = gtk_frame_new(_("История скорости (последние 60 отсчётов)"));
    gtk_container_set_border_width(GTK_CONTAINER(gframe), 2);
    a->graph = gtk_drawing_area_new();
    gtk_widget_set_size_request(a->graph, -1, 150);
    g_signal_connect(a->graph, "draw", G_CALLBACK(on_graph_draw), a);
    gtk_container_add(GTK_CONTAINER(gframe), a->graph);

    /* вертикальный сплиттер: тянем вниз — график растёт за счёт списков сверху */
    a->vpaned = gtk_paned_new(GTK_ORIENTATION_VERTICAL);
    a->hp = hp;
    gtk_paned_pack1(GTK_PANED(a->vpaned), hp, TRUE, TRUE);
    gtk_paned_pack2(GTK_PANED(a->vpaned), gframe, TRUE, FALSE);
    gtk_paned_set_position(GTK_PANED(a->vpaned), 620);
    gtk_box_pack_start(GTK_BOX(vbox), a->vpaned, TRUE, TRUE, 0);

    a->status = GTK_LABEL(gtk_label_new(NULL));
    gtk_label_set_xalign(a->status, 0.0);
    gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(a->status), FALSE, FALSE, 0);

    g_signal_connect(a->win, "delete-event", G_CALLBACK(win_delete_event), NULL);
    g_signal_connect(a->win, "window-state-event", G_CALLBACK(win_state_event), NULL);

    gtk_container_add(GTK_CONTAINER(a->win), vbox);
    gtk_widget_show_all(a->win);
    tray_init();
}

/* ========================= ЖИЗНЕННЫЙ ЦИКЛ ========================= */

/* glob-паттерны паков из секции [groups]: hardware / vpn-server / vpn-client */
static void load_groups(App *a)
{
    if (!a->names) return;
    const char *keys[PACK_N] = { "hardware", "vpn-server", "vpn-client" };
    for (int i = 0; i < PACK_N; i++) {
        char *v = g_key_file_get_string(a->names, "groups", keys[i], NULL);
        if (!v) continue;
        char **pats = g_strsplit_set(v, " \t,", -1);
        for (int j = 0; pats[j]; j++)
            if (pats[j][0]) g_ptr_array_add(a->grp[i], g_strdup(pats[j]));
        g_strfreev(pats);
        g_free(v);
    }
}

static void load_names(App *a)
{
    char *p = g_build_filename(g_get_user_config_dir(), "shaping-view",
                               "class-names.conf", NULL);
    a->names = g_key_file_new();
    if (!g_key_file_load_from_file(a->names, p, G_KEY_FILE_NONE, NULL)) {
        g_key_file_free(a->names);
        a->names = NULL;
    } else {
        load_groups(a);
    }
    g_free(p);
}

static void on_activate(GtkApplication *application, gpointer data)
{
    (void) application;
    App *a = data;
    if (a->win) { gtk_window_present(GTK_WINDOW(a->win)); return; }

    a->tc_path = g_find_program_in_path("tc");
    a->ip_path = g_find_program_in_path("ip");
    load_names(a);
    build_ui(a);

    g_atomic_int_set(&a->worker_run, 1);
    a->worker = g_thread_new("tc-collector", worker_loop, a);
}

static void on_shutdown(GtkApplication *application, gpointer data)
{
    (void) application;
    App *a = data;
    g_atomic_int_set(&a->worker_run, 0);
    if (a->worker) { g_thread_join(a->worker); a->worker = NULL; }
    snapshot_free(a->cur); a->cur = NULL;
    if (a->cards) { g_hash_table_unref(a->cards); a->cards = NULL; }
    if (a->hist)  { g_hash_table_unref(a->hist);  a->hist  = NULL; }
    for (int i = 0; i < PACK_N; i++)
        if (a->grp[i]) { g_ptr_array_unref(a->grp[i]); a->grp[i] = NULL; }
    g_free(a->sel_key); a->sel_key = NULL;
    g_free(a->cards_dev); a->cards_dev = NULL;
    g_free(a->tc_path); a->tc_path = NULL;
    g_free(a->ip_path); a->ip_path = NULL;
    if (a->tray) { g_object_unref(a->tray); a->tray = NULL; }
    if (a->tray_menu) { gtk_widget_destroy(a->tray_menu); a->tray_menu = NULL; }
    if (a->names) { g_key_file_free(a->names); a->names = NULL; }
}

int main(int argc, char **argv)
{
    setlocale(LC_ALL, "");
#if ENABLE_NLS
    bindtextdomain("shaping-view", NULL);
    textdomain("shaping-view");
#endif
    (void) argc; (void) argv;

    memset(&APP, 0, sizeof(APP));
    APP.interval_ms = DEF_INTERVAL_MS;
    APP.cards = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    APP.hist  = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    for (int i = 0; i < PACK_N; i++)
        APP.grp[i] = g_ptr_array_new_with_free_func(g_free);

    APP.app = gtk_application_new(APP_ID, G_APPLICATION_NON_UNIQUE);
    g_signal_connect(APP.app, "activate", G_CALLBACK(on_activate), &APP);
    g_signal_connect(APP.app, "shutdown", G_CALLBACK(on_shutdown), &APP);

    int rc = g_application_run(G_APPLICATION(APP.app), argc, argv);
    g_object_unref(APP.app);
    return rc;
}
