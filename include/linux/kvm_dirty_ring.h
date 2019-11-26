#ifndef KVM_DIRTY_RING_H
#define KVM_DIRTY_RING_H

/**
 * kvm_dirty_ring: KVM internal dirty ring structure
 *
 * @dirty_index: free running counter that points to the next slot in
 *               dirty_ring->dirty_gfns, where a new dirty page should go
 * @reset_index: free running counter that points to the next dirty page
 *               in dirty_ring->dirty_gfns for which dirty trap needs to
 *               be reenabled
 * @size:        size of the compact list, dirty_ring->dirty_gfns
 * @soft_limit:  when the number of dirty pages in the list reaches this
 *               limit, vcpu that owns this ring should exit to userspace
 *               to allow userspace to harvest all the dirty pages
 * @dirty_gfns:  the array to keep the dirty gfns
 * @indices:     the pointer to the @kvm_dirty_ring_indices structure
 *               of this specific ring
 * @index:       index of this dirty ring
 */
struct kvm_dirty_ring {
	u32 dirty_index;
	u32 reset_index;
	u32 size;
	u32 soft_limit;
	struct kvm_dirty_gfn *dirty_gfns;
	struct kvm_dirty_ring_indices *indices;
	int index;
};

u32 kvm_dirty_ring_get_rsvd_entries(void);
int kvm_dirty_ring_alloc(struct kvm_dirty_ring *ring,
			 struct kvm_dirty_ring_indices *indices,
			 int index, u32 size);
struct kvm_dirty_ring *kvm_dirty_ring_get(struct kvm *kvm);
void kvm_dirty_ring_put(struct kvm *kvm,
			struct kvm_dirty_ring *ring);

/*
 * called with kvm->slots_lock held, returns the number of
 * processed pages.
 */
int kvm_dirty_ring_reset(struct kvm *kvm, struct kvm_dirty_ring *ring);

/*
 * returns =0: successfully pushed
 *         <0: unable to push, need to wait
 */
int kvm_dirty_ring_push(struct kvm_dirty_ring *ring, u32 slot, u64 offset);

/* for use in vm_operations_struct */
struct page *kvm_dirty_ring_get_page(struct kvm_dirty_ring *ring, u32 offset);

void kvm_dirty_ring_free(struct kvm_dirty_ring *ring);
bool kvm_dirty_ring_soft_full(struct kvm_dirty_ring *ring);

#endif
