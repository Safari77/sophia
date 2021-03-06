
/*
 * sophia database
 * sphia.org
 *
 * Copyright (c) Dmitry Simonenko
 * BSD License
*/

#include <libss.h>
#include <libsf.h>
#include <libsr.h>
#include <libsv.h>
#include <libsx.h>

static inline int
sx_count(sxmanager *m) {
	return m->count_rd + m->count_rw;
}

int sx_managerinit(sxmanager *m, sr *r, ssa *asxv)
{
	ss_rbinit(&m->i);
	m->count_rd = 0;
	m->count_rw = 0;
	m->csn = 0;
	ss_spinlockinit(&m->lock);
	ss_listinit(&m->indexes);
	m->r = r;
	m->asxv = asxv;
	return 0;
}

int sx_managerfree(sxmanager *m)
{
	assert(sx_count(m) == 0);
	ss_spinlockfree(&m->lock);
	return 0;
}

int sx_indexinit(sxindex *i, sxmanager *m, sr *r, void *ptr)
{
	ss_rbinit(&i->i);
	ss_listinit(&i->link);
	i->dsn = 0;
	i->ptr = ptr;
	i->r = r;
	ss_listappend(&m->indexes, &i->link);
	return 0;
}

int sx_indexset(sxindex *i, uint32_t dsn)
{
	i->dsn = dsn;
	return 0;
}

ss_rbtruncate(sx_truncate,
              sx_vfreeall(((ssa**)arg)[0],
                          ((ssa**)arg)[1], sscast(n, sxv, node)))

static inline void
sx_indextruncate(sxindex *i, sxmanager *m)
{
	if (i->i.root == NULL)
		return;
	ssa *allocators[2] = { m->r->a, m->asxv };
	sx_truncate(i->i.root, allocators);
	ss_rbinit(&i->i);
}

int sx_indexfree(sxindex *i, sxmanager *m)
{
	sx_indextruncate(i, m);
	ss_listunlink(&i->link);
	return 0;
}

uint32_t sx_min(sxmanager *m)
{
	ss_spinlock(&m->lock);
	uint32_t id = 0;
	if (sx_count(m) > 0) {
		ssrbnode *node = ss_rbmin(&m->i);
		sx *min = sscast(node, sx, node);
		id = min->id;
	}
	ss_spinunlock(&m->lock);
	return id;
}

uint32_t sx_max(sxmanager *m)
{
	ss_spinlock(&m->lock);
	uint32_t id = 0;
	if (sx_count(m) > 0) {
		ssrbnode *node = ss_rbmax(&m->i);
		sx *max = sscast(node, sx, node);
		id = max->id;
	}
	ss_spinunlock(&m->lock);
	return id;
}

uint64_t sx_vlsn(sxmanager *m)
{
	ss_spinlock(&m->lock);
	uint64_t vlsn;
	if (sx_count(m) > 0) {
		ssrbnode *node = ss_rbmin(&m->i);
		sx *min = sscast(node, sx, node);
		vlsn = min->vlsn;
	} else {
		vlsn = sr_seq(m->r->seq, SR_LSN);
	}
	ss_spinunlock(&m->lock);
	return vlsn;
}

ss_rbget(sx_matchtx, ss_cmp((sscast(n, sx, node))->id, *(uint32_t*)key))

sx *sx_find(sxmanager *m, uint32_t id)
{
	ssrbnode *n = NULL;
	int rc = sx_matchtx(&m->i, NULL, (char*)&id, sizeof(id), &n);
	if (rc == 0 && n)
		return  sscast(n, sx, node);
	return NULL;
}

void sx_init(sxmanager *m, sx *x)
{
	x->manager = m;
	sv_loginit(&x->log);
	ss_listinit(&x->deadlock);
}

static inline sxstate
sx_promote(sx *x, sxstate state)
{
	x->state = state;
	return state;
}

sxstate sx_begin(sxmanager *m, sx *x, sxtype type, uint64_t vlsn)
{
	sx_promote(x, SXREADY);
	x->type = type;
	x->flags = 0;
	x->log_read = -1;
	sr_seqlock(m->r->seq);
	x->csn = m->csn;
	x->id = sr_seqdo(m->r->seq, SR_TSNNEXT);
	if (sslikely(vlsn == 0))
		x->vlsn = sr_seqdo(m->r->seq, SR_LSN);
	else
		x->vlsn = vlsn;
	sr_sequnlock(m->r->seq);
	sx_init(m, x);
	ss_spinlock(&m->lock);
	ssrbnode *n = NULL;
	int rc = sx_matchtx(&m->i, NULL, (char*)&x->id, sizeof(x->id), &n);
	if (rc == 0 && n) {
		assert(0);
	} else {
		ss_rbset(&m->i, n, rc, &x->node);
	}
	if (type == SXRO)
		m->count_rd++;
	else
		m->count_rw++;
	ss_spinunlock(&m->lock);
	return SXREADY;
}

void sx_gc(sx *x)
{
	sxmanager *m = x->manager;
	sx_promote(x, SXUNDEF);
	sv_logfree(&x->log, m->r->a);
	if (m->count_rw > 0)
		return;
	sslist *p;
	ss_listforeach(&m->indexes, p) {
		sxindex *i = sscast(p, sxindex, link);
		if (i->i.root)
			sx_indextruncate(i, m);
	}
}

static inline void
sx_end(sx *x)
{
	sxmanager *m = x->manager;
	ss_spinlock(&m->lock);
	ss_rbremove(&m->i, &x->node);
	if (x->type == SXRO)
		m->count_rd--;
	else
		m->count_rw--;
	ss_spinunlock(&m->lock);
}

static inline void
sx_rollback_svp(sx *x, ssiter *i, int free)
{
	sxmanager *m = x->manager;
	int gc = 0;
	for (; ss_iterhas(ss_bufiter, i); ss_iternext(ss_bufiter, i))
	{
		svlogv *lv = ss_iterof(ss_bufiter, i);
		sxv *v = lv->v.v;
		/* remove from index and replace head with
		 * a first waiter */
		if (v->prev == NULL) {
			sxindex *i = v->index;
			if (v->next == NULL)
				ss_rbremove(&i->i, &v->node);
			else
				ss_rbreplace(&i->i, &v->node, &v->next->node);
		}
		sx_vunlink(v);
		/* translate log version from sxv to svv */
		sv_init(&lv->v, &sv_vif, v->v, NULL);
		if (free) {
			if (sslikely(! (v->v->flags & SVGET)))
				gc += sv_vsize((svv*)v->v);
			sx_vfree(m->r->a, m->asxv, v);
		}
	}
	ss_quota(m->r->quota, SS_QREMOVE, gc);
}

sxstate sx_rollback(sx *x)
{
	if (x->flags & SXCOMPLETE)
		goto end;
	ssiter i;
	ss_iterinit(ss_bufiter, &i);
	ss_iteropen(ss_bufiter, &i, &x->log.buf, sizeof(svlogv));
	sx_rollback_svp(x, &i, 1);
end:
	sx_promote(x, SXROLLBACK);
	sx_end(x);
	return SXROLLBACK;
}

sxstate sx_prepare(sx *x)
{
	/* proceed read-only transactions */
	if (x->type == SXRO || sv_logcount_write(&x->log) == 0)
		return sx_promote(x, SXPREPARE);
	if (x->flags & SXCONFLICT)
		return sx_promote(x, SXROLLBACK);
	ssiter i;
	ss_iterinit(ss_bufiter, &i);
	ss_iteropen(ss_bufiter, &i, &x->log.buf, sizeof(svlogv));
	for (; ss_iterhas(ss_bufiter, &i); ss_iternext(ss_bufiter, &i))
	{
		svlogv *lv = ss_iterof(ss_bufiter, &i);
		sxv *v = lv->v.v;
		if ((int)v->lo == x->log_read)
			break;
		if (v->prev == NULL)
			continue;
		if (sx_vcommitted(v->prev)) {
			if (v->prev->csn > x->csn)
				return sx_promote(x, SXROLLBACK);
			continue;
		}
		/* force commit for read-only conflicts */
		if (v->prev->v->flags & SVGET)
			continue;
		return sx_promote(x, SXLOCK);
	}
	return sx_promote(x, SXPREPARE);
}

sxstate sx_commit(sx *x)
{
	assert(x->state == SXPREPARE);

	sxmanager *m = x->manager;
	ssiter i;
	ss_iterinit(ss_bufiter, &i);
	ss_iteropen(ss_bufiter, &i, &x->log.buf, sizeof(svlogv));
	uint32_t csn = ++m->csn;
	for (; ss_iterhas(ss_bufiter, &i); ss_iternext(ss_bufiter, &i))
	{
		svlogv *lv = ss_iterof(ss_bufiter, &i);
		sxv *v = lv->v.v;
		if ((int)v->lo == x->log_read)
			break;
		/* abort conflict reader */
		if (v->prev && !sx_vcommitted(v->prev)) {
			assert(v->prev->v->flags & SVGET);
			sx *conflict = sx_find(m, v->prev->id);
			assert(conflict != NULL);
			conflict->flags |= SXCONFLICT;
		}
		/* translate log version from sxv to svv */
		sv_init(&lv->v, &sv_vif, v->v, NULL);
		/* mark stmt as commited */
		sx_vcommit(v, csn);
		sv_vref(v->v);
		/* stmt automatically scheduled for gc */
	}

	/* rollback latest reads */
	sx_rollback_svp(x, &i, 0);

	sx_promote(x, SXCOMMIT);
	sx_end(x);
	return SXCOMMIT;
}

sxstate sx_complete(sx *x)
{
	assert((x->flags & SXCOMPLETE) == 0);
	assert(x->state == SXPREPARE);
	ssiter i;
	ss_iterinit(ss_bufiter, &i);
	ss_iteropen(ss_bufiter, &i, &x->log.buf, sizeof(svlogv));
	sx_rollback_svp(x, &i, 0);
	x->flags |= SXCOMPLETE;
	sx_promote(x, SXCOMMIT);
	sx_end(x);
	return SXCOMMIT;
}

ss_rbget(sx_match,
         sr_compare(scheme, sv_vpointer((sscast(n, sxv, node))->v),
                    (sscast(n, sxv, node))->v->size,
                    key, keysize))

int sx_set(sx *x, sxindex *index, svv *version)
{
	sxmanager *m = x->manager;
	sr *r = m->r;
	if (! (version->flags & SVGET)) {
		x->log_read = -1;
	}
	/* allocate mvcc container */
	sxv *v = sx_valloc(m->asxv, version);
	if (ssunlikely(v == NULL)) {
		sv_vfree(r->a, version);
		return -1;
	}
	v->id = x->id;
	v->index = index;
	svlogv lv;
	lv.id   = index->dsn;
	lv.next = UINT32_MAX;
	sv_init(&lv.v, &sx_vif, v, NULL);
	/* update concurrent index */
	ssrbnode *n = NULL;
	int rc = sx_match(&index->i, index->r->scheme,
	                  sv_vpointer(version),
	                  version->size,
	                  &n);
	if (ssunlikely(rc == 0 && n)) {
		/* exists */
	} else {
		int pos = rc;
		/* unique */
		v->lo = sv_logcount(&x->log);
		rc = sv_logadd(&x->log, r->a, &lv, index->ptr);
		if (ssunlikely(rc == -1)) {
			sr_oom(r->e);
		} else {
			ss_rbset(&index->i, n, pos, &v->node);
		}
		return rc;
	}
	sxv *head = sscast(n, sxv, node);
	/* match previous update made by current
	 * transaction */
	sxv *own = sx_vmatch(head, x->id);
	if (ssunlikely(own))
	{
		if (ssunlikely(version->flags & SVUPDATE)) {
			sr_error(r->e, "%s", "only one update statement is "
			         "allowed per a transaction key");
			sx_vfree(r->a, m->asxv, v);
			return -1;
		}
		/* replace old object with the new one */
		lv.next = sv_logat(&x->log, own->lo)->next;
		v->lo = own->lo;
		sx_vreplace(own, v);
		if (sslikely(head == own))
			ss_rbreplace(&index->i, &own->node, &v->node);
		/* update log */
		sv_logreplace(&x->log, v->lo, &lv);
		ss_quota(r->quota, SS_QREMOVE, sv_vsize(own->v));
		sx_vfree(r->a, m->asxv, own);
		return 0;
	}
	/* update log */
	v->lo = sv_logcount(&x->log);
	rc = sv_logadd(&x->log, r->a, &lv, index->ptr);
	if (ssunlikely(rc == -1)) {
		sx_vfree(r->a, m->asxv, v);
		return sr_oom(r->e);
	}
	/* add version */
	sx_vlink(head, v);
	return 0;
}

int sx_get(sx *x, sxindex *index, sv *key, sv *result)
{
	sxmanager *m = x->manager;
	ssrbnode *n = NULL;
	int rc;
	rc = sx_match(&index->i, index->r->scheme,
	              sv_pointer(key),
	              sv_size(key),
	              &n);
	if (! (rc == 0 && n))
		goto add;
	sxv *head = sscast(n, sxv, node);
	sxv *v = sx_vmatch(head, x->id);
	if (v == NULL)
		goto add;
	if (ssunlikely((v->v->flags & SVGET) > 0))
		return 0;
	if (ssunlikely((v->v->flags & SVDELETE) > 0))
		return 2;
	sv vv;
	sv_init(&vv, &sv_vif, v->v, NULL);
	svv *ret = sv_vdup(m->r->a, &vv);
	if (ssunlikely(ret == NULL)) {
		rc = sr_oom(m->r->e);
	} else {
		sv_init(result, &sv_vif, ret, NULL);
		rc = 1;
	}
	return rc;
add:
	/* track a start of the latest read sequence in the
	 * transactional log */
	if (x->log_read == -1)
		x->log_read = sv_logcount(&x->log);
	rc = sx_set(x, index, key->v);
	if (ssunlikely(rc == -1))
		return -1;
	sv_vref((svv*)key->v);
	return 0;
}

sxstate sx_getstmt(sxmanager *m, sxindex *index ssunused)
{
	sr_seq(m->r->seq, SR_TSNNEXT);
	return SXCOMMIT;
}
