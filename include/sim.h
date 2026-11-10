#ifndef PROCWARDEN_SIM_H
#define PROCWARDEN_SIM_H

/*
 * sim.h — shared definitions between the warden-sim parent (forker) and
 * its children. Everything the children need is passed in one sim_opts
 * struct copied per fork (Lab 3: fork gives each child its own copy).
 */

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define SIM_MAX_CHILDREN 64
#define SIM_MAX_THREADS  64

/* pipe report kinds (children → parent). Writes are one fixed struct, well
 * under PIPE_BUF, so concurrent writers never interleave (Lab 5). */
enum sim_report_kind {
    R_REPORT    = 1,   /* {pid, allocated_mb so far}          */
    R_GRANDKID  = 2,   /* escape child announces its grandchild */
    R_FINAL     = 3,   /* last report before a clean exit       */
};

typedef struct {
    int32_t kind;        /* enum sim_report_kind */
    int32_t mb;          /* allocated MB (R_FINAL: lifetime total) */
    pid_t   pid;
} sim_report;

/* Everything a forked child needs; built by the parent, copied per fork. */
typedef struct {
    const char *profile;         /* install | devserver | watcher | agents */
    int      n_children;
    int      per_child_mb;       /* base allocation per child           */
    int      leak_mb_s;          /* leak rate per child, 0 = none       */
    int      threads;            /* agents: leak threads per child      */
    int      leak_after_s;       /* auto SIGUSR1 after this many seconds */
    int      ignore_hup;         /* SIGHUP -> SIG_IGN instead of stop   */
    int      escape;             /* child 0 spawns a setsid() grandchild */
    int      shared_mb;          /* System V segment size, 0 = none     */
    int      port;               /* devserver TCP listener port, 0 = none */
    int      is_escape_child;    /* set for child 0 when escape is on   */
    int      report_fd;          /* write end of the report pipe        */
    void    *shared_map;         /* inherited shmat mapping, or NULL    */
    size_t   shared_bytes;
} sim_opts;

/* children.c — runs inside a forked child; never returns. */
void sim_child_main(const sim_opts *o);

#endif /* PROCWARDEN_SIM_H */
