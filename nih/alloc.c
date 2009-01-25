/* libnih
 *
 * alloc.c - multi-reference hierarchial allocator
 *
 * Copyright © 2009 Scott James Remnant <scott@netsplit.com>.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif /* HAVE_CONFIG_H */


#include <malloc.h>
#include <stdlib.h>

#include <nih/macros.h>
#include <nih/logging.h>
#include <nih/list.h>

#include "alloc.h"


/**
 * NihAllocCtx:
 * @parents: parents of this context,
 * @children: children of this context,
 * @destructor: function to be called when freed.
 *
 * This structure is placed before all allocations in memory and is used
 * to build up an n-ary tree of them.  Allocations may have multiple
 * parent references and multiple children.  Allocations are automatically
 * freed if the last parent reference is freed.  When an allocation is
 * freed, all children are unreferenced and any destructors called.
 *
 * Members of @parents and @children are both NihAllocRef objects.
 **/
typedef struct nih_alloc_ctx {
	NihList       parents;
	NihList       children;
	NihDestructor destructor;
} NihAllocCtx;

/**
 * NihAllocRef:
 * @children_entry: list head in parent's children list,
 * @parents_entry: list head in child's parents list,
 * @parent: pointer to parent context,
 * @child: pointer to child context.
 *
 * This structure is shared by both @parent and @child denoting a reference
 * between the two of them.  It is placed in @parent's children list through
 * @children_entry and @child's parents list through @parents_entry.
 **/
typedef struct nih_alloc_ref {
	NihList      children_entry;
	NihList      parents_entry;
	NihAllocCtx *parent;
	NihAllocCtx *child;
} NihAllocRef;


/**
 * NIH_ALLOC_CTX:
 * @ptr: pointer to block of memory.
 *
 * Obtain the location of the NihAllocCtx structure given a pointer to the
 * block of memory beyond it.
 *
 * Returns: pointer to NihAllocCtx structure.
 **/
#define NIH_ALLOC_CTX(ptr) ((NihAllocCtx *)(ptr) - 1)

/**
 * NIH_ALLOC_PTR:
 * @ctx: pointer to NihAllocCtx structure.
 *
 * Obtain the location of the block of memory given a pointer to the
 * NihAllocCtx structure in front of it.
 *
 * Returns: pointer to block of memory.
 **/
#define NIH_ALLOC_PTR(ctx) ((void *)((NihAllocCtx *)(ctx) + 1))


/* Prototypes for static functions */
static inline int          nih_alloc_context_free   (NihAllocCtx *ctx);

static inline NihAllocRef *nih_alloc_ref_new        (NihAllocCtx *parent,
						     NihAllocCtx *child)
	__attribute__ ((malloc));
static inline void         nih_alloc_ref_free       (NihAllocRef *ref,
						     int recurse);
static inline NihAllocRef *nih_alloc_ref_lookup     (NihAllocCtx *parent,
						     NihAllocCtx *child);


/* Point to the functions we actually call for allocation. */
void *(*__nih_malloc)(size_t size) = malloc;
void *(*__nih_realloc)(void *ptr, size_t size) = realloc;
void (*__nih_free)(void *ptr) = free;


/**
 * nih_alloc:
 * @parent: parent object for new object,
 * @size: size of requested object.
 *
 * Allocates an object in memory of at least @size bytes and returns a
 * pointer to it.
 *
 * If @parent is not NULL, it should be a pointer to another object which
 * will be used as a parent for the returned object.  When all parents
 * of the returned object are freed, the returned object will also be
 * freed.
 *
 * If you have clean-up that you would like to run, you can assign a
 * destructor using the nih_alloc_set_destructor() function.
 *
 * Returns: newly allocated object or NULL if insufficient memory.
 **/
void *
nih_alloc (const void *parent,
	   size_t      size)
{
	NihAllocCtx *ctx;

	ctx = __nih_malloc (sizeof (NihAllocCtx) + size);
	if (! ctx)
		return NULL;

	nih_list_init (&ctx->parents);
	nih_list_init (&ctx->children);

	ctx->destructor = NULL;

	if (parent)
		nih_alloc_ref_new (NIH_ALLOC_CTX (parent), ctx);

	return NIH_ALLOC_PTR (ctx);
}


/**
 * nih_realloc:
 * @ptr: object to reallocate,
 * @parent: parent object of new object,
 * @size: size of new object.
 *
 * Adjusts the size of the object @ptr to be at least @size bytes, which
 * may be larger or smaller than the existing object, and returns the
 * new pointer.
 *
 * If @ptr is NULL, this simply calls nih_alloc() and passes both @parent
 * and @size to it, returning the returned object.
 *
 * If @ptr is not NULL, @parent is ignored; though it is usual to pass a
 * parent of @ptr for style reasons.
 *
 * Returns: reallocated object or NULL if insufficient memory.
 **/
void *
nih_realloc (void       *ptr,
	     const void *parent,
	     size_t      size)
{
	NihAllocCtx *ctx;
	NihList     *first_parent = NULL;
	NihList     *first_child = NULL;

	if (! ptr)
		return nih_alloc (parent, size);

	ctx = NIH_ALLOC_CTX (ptr);

	/* This is somewhat more difficult than alloc or free because we
	 * have two lists of pointers to worry about.  Fortunately the
	 * properties of NihList help us a lot here.
	 *
	 * The problem is that references between us and our parents,
	 * and references between us and our children, all contain list
	 * pointers that are potentially invalid once relloc has been
	 * called.
	 *
	 * We could strip it all down before calling realloc then rebuild
	 * it afterwards, but that's expensive and could be error-prone in
	 * the case where the allocator fails.
	 *
	 * The solution is to rely on a property of nih_list_add().  The
	 * entry passed (to be added) is cut out of its containing list
	 * without dereferencing the return pointers, this means we can
	 * cut the bad pointers out simply by calling nih_list_add()
	 * to put the new entry back in the same position.
	 *
	 * Of course, this only works in the non-empty list case as trying
	 * to cut an entry out of an empty list would dereference those
	 * invalid pointers.  Happily all we need to do for the empty
	 * list case is call nih_list_init() again.
	 *
	 * So we just remember the first parent and first child reference,
	 * or NULL if the list is empty.
	 */

	if (! NIH_LIST_EMPTY (&ctx->parents))
		first_parent = ctx->parents.next;
	if (! NIH_LIST_EMPTY (&ctx->children))
		first_child = ctx->children.next;

	/* Now do the actual realloc(), if this fails then we can just
	 * return NULL since we've not actually changed anything.
	 */
	ctx = __nih_realloc (ctx, sizeof (NihAllocCtx) + size);
	if (! ctx)
		return NULL;

	/* Now update our parents and children lists, or reinitialise,
	 * as noted above this ensures that all the pointers are correct
	 */
	if (first_parent) {
		nih_list_add (first_parent, &ctx->parents);
	} else {
		nih_list_init (&ctx->parents);
	}

	if (first_child) {
		nih_list_add (first_child, &ctx->children);
	} else {
		nih_list_init (&ctx->children);
	}

	/* We still have to fix up the parent and child pointers, but
	 * that's easy.
	 */
	NIH_LIST_FOREACH (&ctx->parents, iter) {
		NihAllocRef *ref = NIH_LIST_ITER (iter, NihAllocRef,
						  parents_entry);

		ref->child = ctx;
	}

	NIH_LIST_FOREACH (&ctx->children, iter) {
		NihAllocRef *ref = NIH_LIST_ITER (iter, NihAllocRef,
						  children_entry);

		ref->parent = ctx;
	}

	return NIH_ALLOC_PTR (ctx);
}


/**
 * nih_free:
 * @ptr: object to free.
 *
 * Returns the object @ptr to the allocator so the memory consumed may be
 * re-used by something else.
 *
 * All parent references are discarded, the destructor is called, then
 * all children of the object are unreferenced.  Should this be the last
 * reference to any of the children, their destructor will be called and
 * they too will be freed (along with their children, etc.)
 *
 * If you call nih_free() on an object with parent references, you should
 * make sure that any pointers to the object are reset.  If you are unsure
 * whether or not there are references you should call nih_discard(), if
 * you only want to discard a particular parent reference you should call
 * nih_unref().
 *
 * Returns: return value from @ptr's destructor, or 0.
 **/
int
nih_free (void *ptr)
{
	NihAllocCtx *ctx;

	nih_assert (ptr != NULL);

	ctx = NIH_ALLOC_CTX (ptr);

	return nih_alloc_context_free (ctx);
}

/**
 * nih_discard:
 * @ptr: object to discard.
 *
 * If the object @ptr has no parent references, then returns it to the
 * allocator so the memory consumed may be re-used by something else.
 *
 * If @ptr has parent references, this function does nothing and returns.
 *
 * You would use nih_discard() when you allocated @ptr without any parent
 * but have passed it to functions that may have taken a reference to it
 * in the meantime.  Compare with nih_free() which acts even if there are
 * parent references, and nih_unref() which only removes a single parent
 * reference.
 *
 * Returns: return value from @ptr's destructor, or 0.
 **/
int
nih_discard (void *ptr)
{
	NihAllocCtx *ctx;

	nih_assert (ptr != NULL);

	ctx = NIH_ALLOC_CTX (ptr);

	if (NIH_LIST_EMPTY (&ctx->parents))
		return nih_alloc_context_free (ctx);

	return 0;
}

/**
 * _nih_discard_local:
 * @ptr: address of local object to be discarded.
 *
 * This function should never be called directly, it is used as part of the
 * implementation of nih_local and simply calls nih_discard() with the local
 * variable itself.
 **/
void
_nih_discard_local (void *ptraddr)
{
	/* Can't just take void ** as a parameter, since that will upset
	 * gcc typechecking, and we want to be able to be used on any
	 * pointer type.
	 */
	void **ptr = (void **)ptraddr;

	if (*ptr)
		nih_discard (*ptr);
}


/**
 * nih_alloc_context_free:
 * @ctx: context to free.
 *
 * This is the internal function called by nih_free(), nih_discard() and
 * nih_unref() to actually free an allocated context and its attached
 * object.
 *
 * All parent references are discarded, the destructor is called, then
 * all children of the object are unreferenced.  Should this be the last
 * reference to any of the children, their destructor will be called and
 * they too will be freed (along with their children, etc.)
 *
 * Returns: return value from @ptr's destructor, or 0.
 **/
static inline int
nih_alloc_context_free (NihAllocCtx *ctx)
{
	int ret = 0;

	nih_assert (ctx != NULL);

	/* Cast off our parents first, without recursing.  This ensures
	 * we always have zero references before we call the destructor,
	 * and has the somewhat neat property of breaking any reference
	 * loops.
	 */
	NIH_LIST_FOREACH_SAFE (&ctx->parents, iter) {
		NihAllocRef *ref = NIH_LIST_ITER (iter, NihAllocRef,
						  parents_entry);

		nih_alloc_ref_free (ref, FALSE);
	}

	if (ctx->destructor)
		ret = ctx->destructor (NIH_ALLOC_PTR (ctx));

	/* This is safe against other changes to the list, because even if
	 * our child references one of its siblings, we still hold a ref
	 * as well so the sibling won't be freed until we get there.
	 */
	NIH_LIST_FOREACH_SAFE (&ctx->children, iter) {
		NihAllocRef *ref = NIH_LIST_ITER (iter, NihAllocRef,
						  children_entry);

		nih_alloc_ref_free (ref, TRUE);
	}

	__nih_free (ctx);

	return ret;
}


/**
 * nih_alloc_real_set_destructor:
 * @ptr: pointer to object,
 * @destructor: destructor function to set.
 *
 * Sets the destructor of the allocated object @ptr to @destructor, which
 * may be NULL to unset an existing destructor.  Normally you would use
 * the nih_alloc_set_destructor() macro which expands to this function
 * but casts @destructor to the correct type, since almost all destructors
 * will be defined with their argument to be the type of the object
 * rather than void *.
 *
 * The destructor will be called before the object is freed, either
 * explicitly by nih_free() or nih_discard(), or because the last parent
 * has unreferenced the object.
 *
 * When the destructor is called, the parent references to the object will
 * have already been discarded but all children references will be intact
 * and none of the children will have been freed.  There is no need to use
 * a destructor to unreference or free children, that is automatic.
 *
 * The pointer @ptr passed to the destructor is that of the object being
 * freed, and the destructor may return a value which will be the return
 * value of nih_free() or nih_discard() if used directly on the object.
 *
 * Since objects may also be freed by unreferencing, and the value is not
 * returned in this case, it should only be used for informational or
 * debugging purposes.
 **/
void
nih_alloc_real_set_destructor (void          *ptr,
			       NihDestructor  destructor)
{
	NihAllocCtx *ctx;

	nih_assert (ptr != NULL);

	ctx = NIH_ALLOC_CTX (ptr);
	ctx->destructor = destructor;
}


/**
 * nih_ref:
 * @ptr: object to reference,
 * @parent: new parent object.
 *
 * Adds a reference to the object @ptr from @parent, adding to any other
 * objects referencing @ptr.  The reference can be broken using nih_unref().
 *
 * @ptr will only be automatically freed when the last parent unreferences
 * it.  It may still be manually freed with nih_free(), though this doesn't
 * sort out any pointers.
 *
 * This function is generally used when accepting an object that you wish
 * to hold a reference to, which is cheaper than making a copy.  The caller
 * must be careful to only use nih_discard() or nih_unref() to drop its own
 * reference.
 **/
void
nih_ref (void       *ptr,
	 const void *parent)
{
	nih_assert (ptr != NULL);
	nih_assert (parent != NULL);

	nih_alloc_ref_new (NIH_ALLOC_CTX (parent), NIH_ALLOC_CTX (ptr));
}

/**
 * nih_alloc_ref_new:
 * @parent: parent context,
 * @child: child context.
 *
 * This is the internal function used by nih_ref() and nih_alloc() to
 * create a new reference between the @parent and @child contexts.
 *
 * Returns: new reference, already linked to both objects.
 **/
static inline NihAllocRef *
nih_alloc_ref_new (NihAllocCtx *parent,
		   NihAllocCtx *child)
{
	NihAllocRef *ref;

	nih_assert (parent != NULL);
	nih_assert (child != NULL);

	NIH_MUST (ref = malloc (sizeof (NihAllocRef)));

	nih_list_init (&ref->children_entry);
	nih_list_init (&ref->parents_entry);

	ref->parent = parent;
	ref->child = child;

	nih_list_add (&parent->children, &ref->children_entry);
	nih_list_add (&child->parents, &ref->parents_entry);

	return ref;
}


/**
 * nih_unref:
 * @ptr: object to unreference,
 * @parent: parent object to remove.
 *
 * Removes the reference to the object @ptr from @parent, if this is the
 * last reference to @ptr then @ptr will be automatically freed.
 *
 * You never need to call this in your own destructors since children
 * are unreferenced automatically, however this function is useful if you
 * only hold a reference to an object for a short period and wish to
 * discard it.
 **/
void
nih_unref (void       *ptr,
	   const void *parent)
{
	NihAllocRef *ref;

	nih_assert (ptr != NULL);
	nih_assert (parent != NULL);

	ref = nih_alloc_ref_lookup (NIH_ALLOC_CTX (parent),
				    NIH_ALLOC_CTX (ptr));

	nih_assert (ref != NULL);

	nih_alloc_ref_free (ref, TRUE);
}

/**
 * nih_alloc_ref_free:
 * @parent: parent context,
 * @child: child context.
 *
 * This is the internal function used by nih_unref() and
 * nih_alloc_context_free() to remove a reference between the @parent
 * and @child contexts.
 **/
static inline void
nih_alloc_ref_free (NihAllocRef *ref,
		    int          recurse)
{
	nih_assert (ref != NULL);

	nih_list_destroy (&ref->children_entry);
	nih_list_destroy (&ref->parents_entry);

	if (recurse && NIH_LIST_EMPTY (&ref->child->parents))
		nih_alloc_context_free (ref->child);

	free (ref);
}


/**
 * nih_alloc_parent:
 * @ptr: object to query,
 * @parent: parent object to look for.
 *
 * If @parent is NULL any parent will match.
 *
 * Returns: TRUE if @parent has a reference to @ptr, FALSE otherwise.
 **/
int
nih_alloc_parent (const void *ptr,
		  const void *parent)
{
	NihAllocCtx *ctx;

	nih_assert (ptr != NULL);

	ctx = NIH_ALLOC_CTX (ptr);

	if (parent) {
		NihAllocRef *ref;

		ref = nih_alloc_ref_lookup (NIH_ALLOC_CTX (parent), ctx);

		return ref ? TRUE : FALSE;
	} else {
		return NIH_LIST_EMPTY (&ctx->parents) ? FALSE : TRUE;
	}
}

/**
 * nih_alloc_ref_lookup:
 * @parent: parent context,
 * @child: child context.
 *
 * This is the internal function used by nih_unref() and nih_alloc_parent()
 * to lookup a reference between the @parent and @child contexts.
 *
 * Returns: NihAllocRef structure or NULL if no reference exists.
 **/
static inline NihAllocRef *
nih_alloc_ref_lookup (NihAllocCtx *parent,
		      NihAllocCtx *child)
{
	nih_assert (parent != NULL);
	nih_assert (child != NULL);

	NIH_LIST_FOREACH (&child->parents, iter) {
		NihAllocRef *ref = NIH_LIST_ITER (iter, NihAllocRef,
						  parents_entry);

		if (ref->parent == parent)
			return ref;
	}

	return NULL;
}


/**
 * nih_alloc_size:
 * @ptr: pointer to object.
 *
 * Returns: the size of the allocated object, which may be larger than
 * originally requested.
 **/
size_t
nih_alloc_size (const void *ptr)
{
	NihAllocCtx *ctx;

	nih_assert (ptr != NULL);

	ctx = NIH_ALLOC_CTX (ptr);

	return malloc_usable_size (ctx) - sizeof (NihAllocCtx);
}
