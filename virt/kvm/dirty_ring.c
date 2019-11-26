/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * KVM dirty ring implementation
 *
 * Copyright 2019 Red Hat, Inc.
 */
#include <linux/kvm_host.h>
#include <linux/kvm.h>
#include <linux/vmalloc.h>
#include <linux/kvm_dirty_ring.h>
#include <trace/events/kvm.h>

int __weak kvm_cpu_dirty_log_size(void)
{
	return 0;
}

u32 kvm_dirty_ring_get_rsvd_entries(void)
{
	return KVM_DIRTY_RING_RSVD_ENTRIES + kvm_cpu_dirty_log_size();
}

static u32 kvm_dirty_ring_used(struct kvm_dirty_ring *ring)
{
	return READ_ONCE(ring->dirty_index) - READ_ONCE(ring->reset_index);
}

bool kvm_dirty_ring_soft_full(struct kvm_dirty_ring *ring)
{
	return kvm_dirty_ring_used(ring) >= ring->soft_limit;
}

bool kvm_dirty_ring_full(struct kvm_dirty_ring *ring)
{
	return kvm_dirty_ring_used(ring) >= ring->size;
}

struct kvm_dirty_ring *kvm_dirty_ring_get(struct kvm *kvm)
{
	struct kvm_vcpu *vcpu = kvm_get_running_vcpu();

        /*
	 * TODO: Currently use vcpu0 as default ring.  Note that this
	 * should not happen only if called by kvmgt_rw_gpa for x86.
	 * After the kvmgt code refactoring we should remove this,
	 * together with the kvm->dirty_ring_lock.
	 */
	if (!vcpu) {
		pr_warn_once("Detected page dirty without vcpu context. "
			     "Probably because kvm-gt is used. "
			     "May expect unbalanced loads on vcpu0.");
		vcpu = kvm->vcpus[0];
	}

	WARN_ON_ONCE(vcpu->kvm != kvm);

	if (vcpu == kvm->vcpus[0])
		spin_lock(&kvm->dirty_ring_lock);

	return &vcpu->dirty_ring;
}

void kvm_dirty_ring_put(struct kvm *kvm,
			struct kvm_dirty_ring *ring)
{
	struct kvm_vcpu *vcpu = kvm_get_running_vcpu();

	if (!vcpu)
		vcpu = kvm->vcpus[0];

	WARN_ON_ONCE(vcpu->kvm != kvm);
	WARN_ON_ONCE(&vcpu->dirty_ring != ring);

	if (vcpu == kvm->vcpus[0])
		spin_unlock(&kvm->dirty_ring_lock);
}

int kvm_dirty_ring_alloc(struct kvm_dirty_ring *ring,
			 struct kvm_dirty_ring_indices *indices,
			 int index, u32 size)
{
	ring->dirty_gfns = vmalloc(size);
	if (!ring->dirty_gfns)
		return -ENOMEM;
	memset(ring->dirty_gfns, 0, size);

	ring->size = size / sizeof(struct kvm_dirty_gfn);
	ring->soft_limit = ring->size - kvm_dirty_ring_get_rsvd_entries();
	ring->dirty_index = 0;
	ring->reset_index = 0;
	ring->index = index;
	ring->indices = indices;

	return 0;
}

int kvm_dirty_ring_reset(struct kvm *kvm, struct kvm_dirty_ring *ring)
{
	u32 cur_slot, next_slot;
	u64 cur_offset, next_offset;
	unsigned long mask;
	u32 fetch;
	int count = 0;
	struct kvm_dirty_gfn *entry;
	struct kvm_dirty_ring_indices *indices = ring->indices;
	bool first_round = true;

	fetch = READ_ONCE(indices->fetch_index);

	/*
	 * Note that fetch_index is written by the userspace, which
	 * should not be trusted.  If this happens, then it's probably
	 * that the userspace has written a wrong fetch_index.
	 */
	if (fetch - ring->reset_index > ring->size)
		return -EINVAL;

	if (fetch == ring->reset_index)
		return 0;

	/* This is only needed to make compilers happy */
	cur_slot = cur_offset = mask = 0;
	while (ring->reset_index != fetch) {
		entry = &ring->dirty_gfns[ring->reset_index & (ring->size - 1)];
		next_slot = READ_ONCE(entry->slot);
		next_offset = READ_ONCE(entry->offset);
		ring->reset_index++;
		count++;
		/*
		 * Try to coalesce the reset operations when the guest is
		 * scanning pages in the same slot.
		 */
		if (!first_round && next_slot == cur_slot) {
			s64 delta = next_offset - cur_offset;

			if (delta >= 0 && delta < BITS_PER_LONG) {
				mask |= 1ull << delta;
				continue;
			}

			/* Backwards visit, careful about overflows!  */
			if (delta > -BITS_PER_LONG && delta < 0 &&
			    (mask << -delta >> -delta) == mask) {
				cur_offset = next_offset;
				mask = (mask << -delta) | 1;
				continue;
			}
		}
		kvm_reset_dirty_gfn(kvm, cur_slot, cur_offset, mask);
		cur_slot = next_slot;
		cur_offset = next_offset;
		mask = 1;
		first_round = false;
	}
	kvm_reset_dirty_gfn(kvm, cur_slot, cur_offset, mask);

	trace_kvm_dirty_ring_reset(ring);

	return count;
}

int kvm_dirty_ring_push(struct kvm_dirty_ring *ring, u32 slot, u64 offset)
{
	struct kvm_dirty_gfn *entry;
	struct kvm_dirty_ring_indices *indices = ring->indices;

	/*
	 * Note: here we will start waiting even soft full, because we
	 * can't risk making it completely full, since vcpu0 could use
	 * it right after us and if vcpu0 context gets full it could
	 * deadlock if wait with mmu_lock held.
	 */
	if (kvm_get_running_vcpu() == NULL &&
	    kvm_dirty_ring_soft_full(ring))
		return -EBUSY;

	/* It will never gets completely full when with a vcpu context */
	WARN_ON_ONCE(kvm_dirty_ring_full(ring));

	entry = &ring->dirty_gfns[ring->dirty_index & (ring->size - 1)];
	entry->slot = slot;
	entry->offset = offset;
	/*
	 * Make sure the data is filled in before we publish this to
	 * the userspace program.  There's no paired kernel-side reader.
	 */
	smp_wmb();
	ring->dirty_index++;
	WRITE_ONCE(indices->avail_index, ring->dirty_index);

	trace_kvm_dirty_ring_push(ring, slot, offset);

	return 0;
}

struct page *kvm_dirty_ring_get_page(struct kvm_dirty_ring *ring, u32 offset)
{
	return vmalloc_to_page((void *)ring->dirty_gfns + offset * PAGE_SIZE);
}

void kvm_dirty_ring_free(struct kvm_dirty_ring *ring)
{
	vfree(ring->dirty_gfns);
	ring->dirty_gfns = NULL;
}
