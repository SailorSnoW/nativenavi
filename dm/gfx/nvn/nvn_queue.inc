#include "nvn_capture.h"
#include "nvn_hmap.h"

void *objc_autoreleasePoolPush(void);
void objc_autoreleasePoolPop(void *);

// NVN queues share one ordered Metal queue; an unresolved wait parks its queue, never the submitting CPU or another queue.
typedef struct nq_receipt { unsigned refs, published; uint64_t fence, present, copy; } nq_receipt;
typedef struct nq_signal { volatile uint32_t *event; unsigned mode; uint32_t value; } nq_signal;
typedef struct nq_node {
    struct nq_node *next;
    uint8_t *bytes; size_t length, capacity;
    unsigned kind, mode, closed, waiting, ahead; volatile uint32_t *event; uint32_t value;
    uint64_t wait_fence, event_predecessor, wait_since; unsigned wait_kind, wait_said; const sync_t *sync_source; nq_receipt *receipt;
    nq_signal *signals; unsigned signal_count;
    tex_t texture; double period; int crop[4];
} nq_node;
typedef struct nq_owned { struct nq_owned *next; unsigned kind; uint64_t data[]; } nq_owned;
typedef struct nq_job { struct nq_job *next; nq_node *first, *last; nq_receipt *receipt; nq_owned *owned; nvn_hmap objects; uint64_t frame, submitted; } nq_job;
static void nq_job_free(nq_job *j) {
    for(nq_owned *o=j->owned,*next;o;o=next) {
        next=o->next;
        if(o->kind==CAP_K_PROG) mtl_prog_unpin((prog_t *)o->data);
        else if(o->kind==CAP_K_TEX) mtl_tex_unpin(((tex_t *)o->data)->serial);
        free(o);
    }
    free(j->objects.keys); free(j->objects.vals); free(j);
}
static int nq_snapshot_address(void *ctx,uint64_t *field,uint64_t len,int writable) { (void)ctx;(void)field;(void)len;(void)writable;return 1; }
static int nq_snapshot_object(void *ctx,int kind,uint64_t *field,uint8_t *payload,uint32_t size) {
    (void)payload;(void)size;
    nq_job *j=ctx;
    if(!*field || (kind!=CAP_K_PROG && kind!=CAP_K_TEX)) return 1;
    uint64_t key[2]={*field,(unsigned)kind}; nq_owned *o=nvn_hm_get(&j->objects,key);
    if(!o) {
        size_t bytes=kind==CAP_K_PROG?sizeof(prog_t):sizeof(tex_t);
        o=calloc(1,sizeof *o+bytes);
        if(!o) die("queue descriptor allocation failed");
        o->kind=kind; o->next=j->owned; j->owned=o;
        if(kind==CAP_K_PROG) mtl_prog_snapshot((prog_t *)o->data,(const prog_t *)(uintptr_t)*field);
        else {
            memcpy(o->data,(const void *)(uintptr_t)*field,bytes);
            mtl_tex_pin(((tex_t *)o->data)->serial);
        }
        nvn_hm_put(&j->objects,key,o);
    }
    *field=(uintptr_t)o->data;
    return 1;
}
static const cap_ops nq_snapshot_ops={nq_snapshot_address,nq_snapshot_object};
typedef struct nq_queue {
    struct nq_queue *next; uint64_t serial, physical, present, copy, wait_passed; int origin;
    nq_job *first, *last; nq_receipt *tail;
} nq_queue;
static pthread_mutex_t nq_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t nq_cv = PTHREAD_COND_INITIALIZER;
static pthread_once_t nq_once = PTHREAD_ONCE_INIT;
static nq_queue *nq_queues;
static int nq_test_held;
// A wait the queued value of an event's last GPU write satisfies needs no completion; an ADD depends on the writes before it, so it still waits.
typedef struct nq_event_write { struct nq_event_write *next; volatile uint32_t *word; uint64_t fence; uint32_t value; int exact; } nq_event_write;
static nq_event_write *nq_event_writes;
static uint64_t nq_event_pending(volatile uint32_t *word) {
    for(nq_event_write *w=nq_event_writes;w;w=w->next) if(w->word==word) return w->fence;
    return 0;
}
static nq_event_write *nq_event_write_of(volatile uint32_t *word) {
    nq_event_write *w=nq_event_writes;
    while(w && w->word!=word) w=w->next;
    return w;
}
static void nq_event_issued(volatile uint32_t *word,uint64_t fence,unsigned mode,uint32_t value) {
    nq_event_write *w=nq_event_write_of(word);
    if(!w) {
        w=calloc(1,sizeof *w);
        if(!w) die("event ordering allocation failed");
        w->word=word; w->next=nq_event_writes; nq_event_writes=w;
    }
    w->fence=fence; w->value=value; w->exact=mode==0;
}

enum { NQ_COMMANDS, NQ_EVENT, NQ_SYNC, NQ_FENCE, NQ_PRESENT };
static nq_receipt *nq_new(void) {
    nq_receipt *r=calloc(1,sizeof *r);
    if(!r) die("queue receipt allocation failed");
    r->refs=1;
    return r;
}
static void nq_put(nq_receipt *r) { if(r && !--r->refs) free(r); }
static void nq_set_sync(sync_t *s,nq_receipt *r) {
    if(s->fenced==3) nq_put((nq_receipt *)(uintptr_t)s->fence);
    r->refs++; s->fence=(uintptr_t)r; s->fenced=3;
}
void nvn_sync_release(sync_t *s) {
    pthread_mutex_lock(&nq_mu);
    if(s->fenced==3) nq_put((nq_receipt *)(uintptr_t)s->fence);
    s->fenced=0; s->fence=0;
    pthread_mutex_unlock(&nq_mu);
}
static nq_queue *nq_get(queue_t *q) {
    nq_queue *p=nq_queues;
    while(p && p->serial!=q->serial) p=p->next;
    if(!p) {
        p=calloc(1,sizeof *p);
        if(!p) die("queue state allocation failed");
        p->serial=q->serial; p->origin=q->origin_ll; p->next=nq_queues; nq_queues=p;
    }
    return p;
}
static void nq_publish(nq_receipt *r,uint64_t fence) { r->fence=fence; r->published=1; pthread_cond_broadcast(&nq_cv); }
static int nq_predicate(uint32_t current,unsigned mode,uint32_t target) { return mode==0 ? current==target : (uint32_t)(current-target)<0x80000000u; }
static int nq_sync_encoded(const sync_t *s,uint64_t *fence) {
    if(s->fenced==3) {
        nq_receipt *r=(nq_receipt *)(uintptr_t)s->fence;
        if(!r->published) return 0;
        *fence=r->fence;
        return 1;
    }
    if(!s->fenced) return 0;
    *fence=s->fence;
    return 1;
}
void nvn_queue_gpu_progress(void) {
    pthread_mutex_lock(&nq_mu);
    uint64_t done=mtl_completed_fence();
    nq_event_write **p=&nq_event_writes;
    while(*p) {
        nq_event_write *w=*p;
        if(w->fence<=done) { *p=w->next; free(w); }
        else p=&w->next;
    }
    pthread_cond_broadcast(&nq_cv);
    pthread_mutex_unlock(&nq_mu);
}
// Replay ahead takes a wait only on a word the CPU has signalled NQ_AHEAD_SIGNALS times and no command has; a command signal behind a hold ends it in red (mtl_hold_force).
enum { NQ_AHEAD_SIGNALS = 30 };
typedef struct nq_event_src { struct nq_event_src *next; volatile uint32_t *word; unsigned cpu, gpu; } nq_event_src;
static nq_event_src *nq_event_srcs;
static int nq_event_cpu(volatile uint32_t *word) {
    for(nq_event_src *e=nq_event_srcs;e;e=e->next) if(e->word==word) return e->cpu>=NQ_AHEAD_SIGNALS && !e->gpu;
    return 0;
}
static nq_event_src *nq_event_src_get(volatile uint32_t *word) {
    nq_event_src *e=nq_event_srcs;
    while(e && e->word!=word) e=e->next;
    if(!e) {
        e=calloc(1,sizeof *e);
        if(!e) die("event source allocation failed");
        e->word=word; e->next=nq_event_srcs; nq_event_srcs=e;
    }
    return e;
}
static void nq_event_src_note(volatile uint32_t *word,int cpu) {
    nq_event_src *e=nq_event_src_get(word);
    if(cpu) { if(e->cpu<NQ_AHEAD_SIGNALS) e->cpu++; }
    else e->gpu=1;
}
void nvn_queue_ahead_refuse(volatile uint32_t *word) {
    pthread_mutex_lock(&nq_mu);
    nq_event_src_get(word)->cpu=0;
    pthread_mutex_unlock(&nq_mu);
}
static void nq_event_publish(volatile uint32_t *word,unsigned mode,uint32_t value,uint64_t gpu_fence) {
    pthread_mutex_lock(&nq_mu);
    nq_event_src_note(word,!gpu_fence);
    if(mode==1) atomic_fetch_add_explicit((_Atomic uint32_t *)word,value,memory_order_release);
    else atomic_store_explicit((_Atomic uint32_t *)word,value,memory_order_release);
    atomic_thread_fence(memory_order_seq_cst);
    // a later signal must not erase a release
    for(nq_queue *q=nq_queues;q;q=q->next) if(q->first && q->first->first) {
        nq_node *n=q->first->first;
        if(n->kind==NQ_EVENT && n->waiting && n->event==word && (gpu_fence ? gpu_fence>=n->event_predecessor : nq_event_pending(word)<=mtl_completed_fence()) &&
           nq_predicate(atomic_load_explicit((_Atomic uint32_t *)word,memory_order_acquire),n->mode,n->value)) { n->event=NULL; n->wait_fence=gpu_fence; }
    }
    uint32_t left=atomic_load_explicit((_Atomic uint32_t *)word,memory_order_acquire);
    pthread_cond_broadcast(&nq_cv);
    pthread_mutex_unlock(&nq_mu);
    mtl_hold_signal(word,left);
}
void nvn_event_publish(volatile uint32_t *word,unsigned mode,uint32_t value) { nq_event_publish(word,mode,value,0); }
void nvn_event_publish_gpu(volatile uint32_t *word,unsigned mode,uint32_t value,uint64_t fence) { nq_event_publish(word,mode,value,fence); }
static void nq_snapshot_sync(nq_node *n,const sync_t *s) {
    n->wait_kind=s->fenced; n->wait_fence=s->fence;
    if(s->fenced==3) {
        n->receipt=(nq_receipt *)(uintptr_t)s->fence;
        n->receipt->refs++;
    }
}
typedef struct { uint64_t wall, cpu, instr, cycles, sys; } nq_cost;
static nq_cost nq_cost_begin(void) {
    nq_cost c={.wall=now_ns(),.cpu=clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID),.sys=thr_sys_ns()};
    thr_counts(&c.instr,&c.cycles);
    return c;
}
static void nq_cost_end(nq_cost c) {
    uint64_t dw=now_ns()-c.wall, dc=clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID)-c.cpu, ins, cyc;
    thr_counts(&ins,&cyc);
    atomic_fetch_add(&g_nvn_ns,dw); atomic_fetch_add(&g_sub_wall_ns,dw); atomic_fetch_add(&g_cum_sub_wall_ns,dw);
    atomic_fetch_add(&g_sub_cpu_ns,dc); atomic_fetch_add(&g_cum_sub_cpu_ns,dc);
    atomic_fetch_add(&g_sub_instr,ins-c.instr); atomic_fetch_add(&g_cum_sub_instr,ins-c.instr);
    atomic_fetch_add(&g_cum_sub_cyc,cyc-c.cycles); atomic_fetch_add(&g_cum_sub_sys_ns,thr_sys_ns()-c.sys);
}
// A reset drops every job not replayed yet, as a process exit does: a wait the stopped game was to signal would hold its queue for good.
// Only the replay thread may do it: a node it took may still be on its way to g_sub_mu.
static int nq_drop_req, nq_drop_done;
static void nq_start(void);
static void nq_drop_all(void) {   // replay thread, holding nq_mu; returns holding it
    nq_job *jobs = NULL;
    nq_node *pres = NULL;
    unsigned nj = 0;
    for (nq_queue *q = nq_queues; q; q = q->next) {
        for (nq_job *j = q->first, *jn; j; j = jn) {
            jn = j->next;
            nj++;
            for (nq_node *n = j->first, *nn; n; n = nn) {
                nn = n->next;
                if (n->kind == NQ_FENCE) nq_publish(n->receipt, q->physical);
                if (n->receipt) nq_put(n->receipt);
                free(n->signals);
                free(n->bytes);
                if (n->kind == NQ_PRESENT) {
                    n->next = pres;
                    pres = n;
                } else free(n);
            }
            j->first = j->last = NULL;
            nq_publish(j->receipt, q->physical);
            nq_put(j->receipt);
            j->next = jobs;
            jobs = j;
        }
        q->first = q->last = NULL;
    }
    pthread_mutex_unlock(&nq_mu);
    pthread_mutex_lock(&g_sub_mu);
    void *pool = objc_autoreleasePoolPush();
    for (nq_job *j = jobs, *jn; j; j = jn) {
        jn = j->next;
        nq_job_free(j);
    }
    for (nq_node *n = pres, *nn; n; n = nn) {
        nn = n->next;
        mtl_tex_unpin(n->texture.serial);
        free(n);
    }
    objc_autoreleasePoolPop(pool);
    pthread_mutex_unlock(&g_sub_mu);
    pthread_mutex_lock(&nq_mu);
    if (nj) LOG("reset: %u queued NVN jobs dropped", nj);
}
void nvn_queue_reset_drop(void) {
    pthread_once(&nq_once, nq_start);
    pthread_mutex_lock(&nq_mu);
    nq_drop_req = 1;
    nq_drop_done = 0;
    pthread_cond_broadcast(&nq_cv);
    while (!nq_drop_done) pthread_cond_wait(&nq_cv, &nq_mu);
    nq_drop_req = 0;
    for (nq_event_write *w = nq_event_writes, *n; w; w = n) {
        n = w->next;
        free(w);
    }
    for (nq_event_src *e = nq_event_srcs, *n; e; e = n) {
        n = e->next;
        free(e);
    }
    nq_event_writes = NULL;
    nq_event_srcs = NULL;
    pthread_mutex_unlock(&nq_mu);
}
static void *nq_worker(void *unused) {
    (void)unused;
    pthread_setname_np("NVN queue replay");
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE,0);
    void *rt=nvn_rt_join("queue replay thread");
    uint64_t frame=UINT64_MAX;
    for(;;) {
        pthread_mutex_lock(&nq_mu);
        nq_queue *q=NULL; nq_node *n=NULL; nq_job *j=NULL;
        for(;;) {
        scan_again:;
            if(nq_drop_req && !nq_drop_done) {
                nq_drop_all();
                nq_drop_done=1;
                pthread_cond_broadcast(&nq_cv);
            }
            if(nq_test_held) { pthread_cond_wait(&nq_cv,&nq_mu); continue; }
            int mapped_event_wait=0; nq_node *ahead=NULL;
            for(q=nq_queues;q;q=q->next) {
                j=q->first;
                if(!j) continue;
                n=j->first;
                if(!n) {
                    nq_publish(j->receipt,q->physical);
                    q->first=j->next;
                    if(!q->first) q->last=NULL;
                    nq_put(j->receipt);
                    pthread_mutex_unlock(&nq_mu);
                    pthread_mutex_lock(&g_sub_mu);
                    void *pool=objc_autoreleasePoolPush();
                    nq_job_free(j);
                    objc_autoreleasePoolPop(pool);
                    pthread_mutex_unlock(&g_sub_mu);
                    pthread_mutex_lock(&nq_mu);
                    goto scan_again;
                }
                if(n->kind==NQ_EVENT && n->event && n->ahead) {
                    if(mtl_hold_active()) break;
                    n->ahead=0;
                }
                if(n->kind==NQ_EVENT && n->event) {
                    uint64_t predecessor=nq_event_pending(n->event);
                    if(!n->waiting) n->event_predecessor=predecessor;
                    // the queued GPU write satisfies the wait: the GPU runs this after it
                    nq_event_write *queued=predecessor>mtl_completed_fence() ? nq_event_write_of(n->event) : NULL;
                    if(queued && queued->exact && nq_predicate(queued->value,n->mode,n->value)) break;
                    if(predecessor>mtl_completed_fence() || !nq_predicate(atomic_load_explicit((_Atomic uint32_t *)n->event,memory_order_acquire),n->mode,n->value)) {
                        uint64_t t=now_ns();
                        if(!n->wait_since) n->wait_since=t;
                        else if(t-n->wait_since>5000000000ull && !n->wait_said) {
                            static _Atomic int said;
                            n->wait_said=1;
                            if(atomic_fetch_add(&said,1)<8)
                                LOG("[RED] queue %llu: an event wait has held it %.1f s: word %p is %#x, waiting for %s %#x; a GPU write of it is %s",(unsigned long long)q->serial,(t-n->wait_since)/1e9,(void *)n->event,
                                    atomic_load_explicit((_Atomic uint32_t *)n->event,memory_order_acquire),n->mode==0?"equal to":"at least",n->value,predecessor>mtl_completed_fence()?"queued, not completed":"not queued");
                        }
                        if(!ahead && predecessor<=mtl_completed_fence() && (n->next || j->next) && nq_event_cpu(n->event)) ahead=n;
                        n->waiting=1;
                        mapped_event_wait=1;
                        continue;
                    }
                }
                if(n->kind==NQ_SYNC) {
                    if(n->receipt && !n->receipt->published) {
                        uint64_t t=now_ns();
                        if(!n->wait_since) n->wait_since=t;
                        else if(t-n->wait_since>5000000000ull && !n->wait_said) {
                            static _Atomic int said;
                            n->wait_said=1;
                            if(atomic_fetch_add(&said,1)<8)
                                LOG("[RED] queue %llu: a sync wait has held it %.1f s: the fence it waits for (receipt %p) is not published yet",(unsigned long long)q->serial,(t-n->wait_since)/1e9,(void *)n->receipt);
                        }
                        continue;
                    }
                    // A sync not yet fenced when the wait was submitted (wait_kind 0) passes, as on the console.
                }
                break;
            }
            if(q) break;
            // Only this thread takes nodes: the node stays this queue's first while the lock is dropped.
            if(ahead && mtl_replay_ahead() && !mtl_hold_active()) {
                volatile uint32_t *word=ahead->event; unsigned mode=ahead->mode; uint32_t value=ahead->value;
                pthread_mutex_unlock(&nq_mu);
                dm_host_mutex_lock(&g_sub_mu);
                int held=mtl_hold_begin(word,mode,value);
                pthread_mutex_unlock(&g_sub_mu);
                pthread_mutex_lock(&nq_mu);
                if(held) {
                    ahead->ahead=1;
                    if(!ahead->event) mtl_hold_satisfied();
                    goto scan_again;
                }
            }
            // an event word may change by a plain store to mapped pool memory: poll
            if(mapped_event_wait) {
                struct timespec tick={0,1000000};
                pthread_cond_timedwait_relative_np(&nq_cv,&nq_mu,&tick);
            }
            else pthread_cond_wait(&nq_cv,&nq_mu);
        }
        j->first=n->next;
        if(!j->first) j->last=NULL;
        // round-robin, so a continually fed producer cannot starve another queue
        if(q->next) {
            nq_queue **link=&nq_queues;
            while(*link!=q) link=&(*link)->next;
            *link=q->next;
            nq_queue *tail=nq_queues;
            while(tail->next) tail=tail->next;
            tail->next=q;
            q->next=NULL;
        }
        if(n->kind==NQ_SYNC || n->kind==NQ_EVENT) {
            uint64_t f=n->receipt?n->receipt->fence:n->wait_fence;
            if(f>q->physical) q->physical=f;
        }
        if((n->kind==NQ_SYNC || n->kind==NQ_EVENT) && n->wait_since) q->wait_passed=guest_ns();
        if(n->kind==NQ_FENCE) nq_publish(n->receipt,q->physical);
        pthread_mutex_unlock(&nq_mu);
        uint64_t fence=0, present=0, copy=0;
        if(n->kind==NQ_COMMANDS || n->kind==NQ_PRESENT) {
            nvn_rt_tick(rt);
            dm_host_mutex_lock(&g_sub_mu);
            void *pool=objc_autoreleasePoolPush();
            dm_host_work++; mtl_prepare(); dm_host_work--;
            gfx_enter();
            nq_cost cost=nq_cost_begin();
            if(frame!=j->frame) { frame=j->frame; mtl_set_frame(frame); }
            mtl_queue_activate(q->serial);
            mtl_set_queue_origin_lower_left(q->origin);
            if(n->kind==NQ_PRESENT) mtl_hold_wait();
            if(n->kind==NQ_COMMANDS) {
                uint64_t handle=(uintptr_t)n->bytes;
                nvn_cap_submit(1,&handle);
                mtl_sub_begin();
                mtl_sub_ready(j->submitted>q->wait_passed?j->submitted:q->wait_passed);
                nvn_ft_poll();
                mtl_exec_rest(NULL,0);
                mtl_exec(n->bytes);
                fence=mtl_sub_commit();
            } else {
                nvn_cap_present(&n->texture,n->crop);
                present=mtl_present(&n->texture,n->period,n->crop,&copy);
                mtl_tex_unpin(n->texture.serial);
            }
            nq_cost_end(cost);
            gfx_leave();
            objc_autoreleasePoolPop(pool);
            pthread_mutex_unlock(&g_sub_mu);
        }
        pthread_mutex_lock(&nq_mu);
        if(n->kind==NQ_PRESENT) { q->present=present; q->copy=copy; j->receipt->present=present; j->receipt->copy=copy; }
        if(fence) {
            q->physical=fence;
            for(unsigned i=0;i<n->signal_count;i++) nq_event_issued(n->signals[i].event,fence,n->signals[i].mode,n->signals[i].value);
        }
        if(n->receipt) nq_put(n->receipt);
        free(n->signals);
        free(n->bytes);
        free(n);
        pthread_cond_broadcast(&nq_cv);
        pthread_mutex_unlock(&nq_mu);
    }
    return NULL;
}
static void nq_start(void) {
    pthread_t t;
    if(pthread_create(&t,NULL,nq_worker,NULL)) die("NVN queue worker failed");
    pthread_detach(t);
}
static nq_node *nq_append(nq_job *j,unsigned kind) {
    nq_node *n=calloc(1,sizeof *n);
    if(!n) die("queue continuation allocation failed");
    n->kind=kind;
    if(j->last) j->last->next=n; else j->first=n;
    j->last=n;
    return n;
}
static void nq_bytes(nq_node *n, const void *bytes, size_t length) {
    if (length > SIZE_MAX - n->length) die("queue commands allocation overflow");
    size_t want = n->length + length;
    if (want > n->capacity) {
        size_t cap = n->capacity <= SIZE_MAX / 2 ? 2 * n->capacity : want;
        cap = cap < want ? want : cap;
        if (cap < 4096) cap = 4096;
        void *grown = realloc(n->bytes, cap);
        if (!grown) die("queue commands allocation failed");
        n->bytes = grown;
        n->capacity = cap;
    }
    memcpy(n->bytes + n->length, bytes, length);
    n->length = want;
}
static void nq_signal_add(nq_node *n,nq_signal s) {
    if(!(n->signal_count&(n->signal_count-1))) {
        n->signals=realloc(n->signals,(n->signal_count?2*n->signal_count:1)*sizeof *n->signals);   // NOLINT(bugprone-suspicious-realloc-usage): failure aborts
        if(!n->signals) die("queue signals allocation failed");
    }
    n->signals[n->signal_count++]=s;
}
static void nq_end(nq_job *j) {
    if(j->last && j->last->kind==NQ_COMMANDS && !j->last->closed) {
        op_hdr end={OP_END,8};
        nq_bytes(j->last,&end,8);
        j->last->closed=1;
    }
}
// Waits and fences cut the job; a signal does not (its wait is ordered by value or by the fence of its commands).
// Each node is a Metal command buffer, so a cut also ends the open render pass.
static void nq_record_one(nq_job *j,const uint8_t *p,op_hdr h) {
    const uint8_t *a=p+8;
    if(h.op==OP_END || h.op==OP_JUMP || h.op==OP_CALL) die("NVN command list: a %s record inside a recording's piece at %p",h.op==OP_END?"END":h.op==OP_JUMP?"JUMP":"CALL",(const void *)p);
    if(h.op==OP_WAIT_EVENT || h.op==OP_WAIT_SYNC || h.op==OP_FENCE_SYNC) {
        nq_end(j);
        nq_node *n=nq_append(j,h.op==OP_WAIT_EVENT?NQ_EVENT:h.op==OP_WAIT_SYNC?NQ_SYNC:NQ_FENCE);
        if(h.op==OP_WAIT_EVENT) { memcpy(&n->event,a,8); memcpy(&n->mode,a+8,4); memcpy(&n->value,a+12,4); }
        else {
            memcpy(&n->sync_source,a,8);
            if(h.op==OP_FENCE_SYNC) n->receipt=nq_new();
        }
    } else {
        nq_node *n=j->last;
        if(!n || n->kind!=NQ_COMMANDS || n->closed) n=nq_append(j,NQ_COMMANDS);
        size_t offset=n->length;
        nq_bytes(n,p,h.size);
        if(!n->bytes) die("queue commands: a record of %u bytes",h.size);
        if(!cap_rec_fields(h.op,n->bytes+offset+8,h.size,&nq_snapshot_ops,j)) die("queue snapshot: unknown command %u",h.op);
        if(h.op==OP_SIGNAL_EVENT) {
            nq_signal sig;
            memcpy(&sig.event,a,8); memcpy(&sig.mode,a+8,4); memcpy(&sig.value,a+12,4);
            nq_signal_add(n,sig);
        }
    }
}
static void nq_record(nq_job *j,const uint8_t *p,op_hdr h) {
    if(!rec_compact(h.op)) { nq_record_one(j,p,h); return; }
    uint8_t x[REC_EXPAND_MAX] __attribute__((aligned(8)));
    size_t n=rec_expand(p,x);
    for(size_t o=0;o<n;) {
        op_hdr c;
        memcpy(&c,x+o,8);
        nq_record_one(j,x+o,c);
        o+=c.size;
    }
}
// A piece runs what its command memory holds at submit (docs/reference/nvn-objects.md, "A called recording recorded over before its caller is submitted").
static void nq_flatten(nq_job *j,const uint8_t *list) {
    for(const uint8_t *e=list;;e+=8) {
        uint64_t v;
        memcpy(&v,e,8);
        unsigned k=ctl_kind(v);
        if(k==CTL_LINK) { e=ctl_addr(v)-8; continue; }
        if(k!=CTL_CHUNK) die("NVN command list %p: corrupt control entry %#llx at %p",(const void *)list,(unsigned long long)v,(const void *)e);
        const uint8_t *p=ctl_addr(v),*pe=p+ctl_chunk_bytes(v);
        op_hdr h;
        while(nvn_piece_record(list,v,p,pe,&h)) { nq_record(j,p,h); p+=h.size; }
        if(v&CTL_LAST) return;
    }
}
static void nq_enqueue(nq_queue *q,nq_job *j) {
    for(nq_node *n=j->first;n;n=n->next)
        for(unsigned i=0;i<n->signal_count;i++) {   // a hold on it now could not be satisfied
            nq_event_src_note(n->signals[i].event,0);
            mtl_hold_force(n->signals[i].event);
        }
    if(q->last) q->last->next=j; else q->first=j;
    q->last=j;
    nq_put(q->tail);
    q->tail=j->receipt;
    q->tail->refs++;
    pthread_cond_broadcast(&nq_cv);
}
void nvn_queue_submit(queue_t *q,int count,const uint64_t *handles) {
    pthread_once(&nq_once,nq_start);
    nq_job *j=calloc(1,sizeof *j);
    if(!j) die("queue job allocation failed");
    j->receipt=nq_new(); j->objects.klen=16; j->frame=dm_nvn_frame_count(); j->submitted=guest_ns();
    // no scheduler lock while copying descriptors or resolving shader libraries
    dm_host_mutex_lock(&g_sub_mu);
    void *pool=objc_autoreleasePoolPush();
    dm_host_work++; mtl_prepare(); dm_host_work--;
    nq_cost cost=nq_cost_begin();
    for(int i=0;i<count;i++) nq_flatten(j,(const uint8_t *)(uintptr_t)handles[i]);
    nq_end(j);
    nq_cost_end(cost);
    objc_autoreleasePoolPop(pool);
    pthread_mutex_unlock(&g_sub_mu);
    int waits=0;
    for(nq_node *node=j->first;node;node=node->next) waits|=node->kind==NQ_EVENT || node->kind==NQ_SYNC;
    nvn_cap_queue(q->serial,waits);
    pthread_mutex_lock(&nq_mu);
    for(nq_node *node=j->first;node;node=node->next) if(node->sync_source) {
        if(node->kind==NQ_FENCE) nq_set_sync((sync_t *)node->sync_source,node->receipt); else nq_snapshot_sync(node,node->sync_source);
        node->sync_source=NULL;
    }
    nq_enqueue(nq_get(q),j);
    pthread_mutex_unlock(&nq_mu);
}
// Only replay turns this receipt into a display ticket.
uint64_t nvn_queue_present(queue_t *q,const tex_t *t,double period,const int crop[4]) {
    nvn_cap_queue(q->serial,0);
    pthread_once(&nq_once,nq_start);
    nq_job *j=calloc(1,sizeof *j);
    if(!j) die("queue present allocation failed");
    j->receipt=nq_new();
    j->frame=dm_nvn_frame_count();
    { void mtl_present_recorded(uint64_t); mtl_present_recorded(j->frame); }
    nq_node *n=nq_append(j,NQ_PRESENT);
    n->texture=*t; mtl_tex_pin(t->serial); n->period=period;
    if(crop) memcpy(n->crop,crop,sizeof n->crop);
    pthread_mutex_lock(&nq_mu);
    j->receipt->refs++;
    uint64_t ticket=(uintptr_t)j->receipt;
    nq_enqueue(nq_get(q),j);
    pthread_mutex_unlock(&nq_mu);
    return ticket;
}
void nvn_queue_test_hold(int on) {
    pthread_mutex_lock(&nq_mu);
    nq_test_held=on;
    pthread_cond_broadcast(&nq_cv);
    pthread_mutex_unlock(&nq_mu);
}
void nvn_queue_present_release(uint64_t ticket) {
    if(!ticket) return;
    pthread_mutex_lock(&nq_mu);
    nq_put((nq_receipt *)(uintptr_t)ticket);
    pthread_mutex_unlock(&nq_mu);
}
void nvn_queue_present_wait(uint64_t ticket) {
    if(!ticket) return;
    nq_receipt *r=(nq_receipt *)(uintptr_t)ticket;
    pthread_mutex_lock(&nq_mu);
    while(!r->published) pthread_cond_wait(&nq_cv,&nq_mu);
    uint64_t physical=r->present, copy=r->copy;
    pthread_mutex_unlock(&nq_mu);
    if(physical) mtl_present_wait_texture(physical,copy);
}
void nvn_queue_fence(queue_t *q,sync_t *s) {
    pthread_mutex_lock(&nq_mu);
    nq_queue *p=nq_get(q);
    if(p->tail) nq_set_sync(s,p->tail);
    else {
        if(s->fenced==3) nq_put((nq_receipt *)(uintptr_t)s->fence);
        s->fenced=1; s->fence=p->physical;
    }
    pthread_mutex_unlock(&nq_mu);
}
int nvn_sync_wait(const sync_t *s,uint64_t timeout) {
    uint64_t start=now_ns(),fence=0;
    pthread_mutex_lock(&nq_mu);
    while(!nq_sync_encoded(s,&fence)) {
        uint64_t elapsed=now_ns()-start;
        if(timeout!=UINT64_MAX && elapsed>=timeout) { pthread_mutex_unlock(&nq_mu); return 0; }
        if(timeout==UINT64_MAX) pthread_cond_wait(&nq_cv,&nq_mu);
        else {
            uint64_t left=timeout-elapsed;
            struct timespec ts={(time_t)(left/1000000000ull),(long)(left%1000000000ull)};
            pthread_cond_timedwait_relative_np(&nq_cv,&nq_mu,&ts);
        }
    }
    pthread_mutex_unlock(&nq_mu);
    uint64_t elapsed=now_ns()-start;
    return mtl_wait(fence,timeout==UINT64_MAX?timeout:elapsed>=timeout?0:timeout-elapsed);
}
void nvn_queue_wait_sync(queue_t *q,const sync_t *s) {
    nvn_cap_queue(q->serial,1);
    pthread_once(&nq_once,nq_start);
    nq_job *j=calloc(1,sizeof *j);
    if(!j) die("queue wait allocation failed");
    j->receipt=nq_new();
    pthread_mutex_lock(&nq_mu);
    nq_snapshot_sync(nq_append(j,NQ_SYNC),s);
    nq_enqueue(nq_get(q),j);
    pthread_mutex_unlock(&nq_mu);
}
void nvn_queue_finish(queue_t *q) {
    sync_t s={0};
    nvn_queue_fence(q,&s);
    nvn_sync_wait(&s,UINT64_MAX);
    nvn_sync_release(&s);
    pthread_mutex_lock(&nq_mu);
    nq_queue *p=nq_get(q);
    uint64_t present=p->present, copy=p->copy;
    pthread_mutex_unlock(&nq_mu);
    if(present) mtl_present_wait_texture(present,copy);   // a snapshot present waits for its copy, not the screen
}
void nvn_queue_forget(queue_t *q) {
    nvn_queue_finish(q);
    pthread_mutex_lock(&nq_mu);
    nq_queue **p=&nq_queues;
    while(*p && (*p)->serial!=q->serial) p=&(*p)->next;
    if(*p) {
        nq_queue *old=*p;
        *p=old->next;
        nq_put(old->tail);
        free(old);
    }
    pthread_mutex_unlock(&nq_mu);
}
