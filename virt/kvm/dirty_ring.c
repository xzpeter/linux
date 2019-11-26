#include <linux/kvm_host.h>
#include <linux/kvm.h>
#include <linux/vmalloc.h>
#include <linux/kvm_dirty_ring.h>

u32 kvm_dirty_ring_get_rsvd_entries(void)
{
	return KVM_DIRTY_RING_RSVD_ENTRIES + kvm_cpu_dirty_log_size();
}

int kvm_dirty_ring_alloc(struct kvm_vcpu *vcpu)
{
	struct kvm_dirty_ring *dirty_ring = &vcpu->dirty_ring;
	struct kvm *kvm = vcpu->kvm;

	u32 size = vcpu->kvm->dirty_ring_size;

	dirty_ring->dirty_gfns = vmalloc(size);
	if (!dirty_ring->dirty_gfns)
		return -ENOMEM;
	memset(dirty_ring->dirty_gfns, 0, size);

	dirty_ring->size = size / sizeof(struct kvm_dirty_gfn);
	dirty_ring->soft_limit =
	    (kvm->dirty_ring_size / sizeof(struct kvm_dirty_gfn)) -
	    kvm_dirty_ring_get_rsvd_entries();
	dirty_ring->dirty_index = 0;
	dirty_ring->reset_index = 0;
	spin_lock_init(&dirty_ring->lock);

	return 0;
}

static u32 kvm_dirty_ring_reset_index(struct kvm_dirty_ring *ring)
{
	return ring->reset_index & (ring->size - 1);
}

int kvm_dirty_ring_reset(struct kvm_vcpu *vcpu)
{
	u32 cur_slot, next_slot;
	u64 cur_offset, next_offset;
	unsigned long mask;
	u32 fetch;
	int count = 0;
	struct kvm_dirty_gfn *entry;
	struct kvm_dirty_ring *ring = &vcpu->dirty_ring;
	struct kvm_run *run = vcpu->run;
	struct kvm *kvm = vcpu->kvm;

	fetch = READ_ONCE(run->dirty_ring_indexes.fetch_index);
	fetch &= (ring->size - 1);

	if (fetch == kvm_dirty_ring_reset_index(ring))
		return 0;

	entry = &ring->dirty_gfns[kvm_dirty_ring_reset_index(ring)];
	/*
	 * The ring buffer is shared with userspace, which might mmap
	 * it and concurrently modify slot and offset.  Userspace must
	 * not be trusted!  READ_ONCE prevents the compiler from changing
	 * the values after they've been range-checked (the checks are
	 * in kvm_reset_dirty_gfn).
	 */
	smp_read_barrier_depends();
	cur_slot = READ_ONCE(entry->slot);
	cur_offset = READ_ONCE(entry->offset);
	mask = 1;
	count++;
	ring->reset_index++;
	while (kvm_dirty_ring_reset_index(ring) != fetch) {
		entry = &ring->dirty_gfns[kvm_dirty_ring_reset_index(ring)];
		smp_read_barrier_depends();
		next_slot = READ_ONCE(entry->slot);
		next_offset = READ_ONCE(entry->offset);
		ring->reset_index++;
		count++;
		/*
		 * Try to coalesce the reset operations when the guest is
		 * scanning pages in the same slot.
		 */
		if (next_slot == cur_slot) {
			int delta = next_offset - cur_offset;

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
	}
	kvm_reset_dirty_gfn(kvm, cur_slot, cur_offset, mask);

	return count;
}

int kvm_dirty_ring_push(struct kvm_vcpu *vcpu, u32 slot, u64 offset)
{
	int ret;
	u16 num;
	struct kvm_dirty_ring *dirty_ring = &vcpu->dirty_ring;
	struct kvm_dirty_gfn *entry;

	spin_lock(&dirty_ring->lock);

	num = (u16)(dirty_ring->dirty_index - dirty_ring->reset_index);
	if (num >= dirty_ring->size) {
		WARN_ON_ONCE(num > dirty_ring->size);
		ret = -EBUSY;
		goto out;
	}

	entry = &dirty_ring->dirty_gfns[dirty_ring->dirty_index &
			(dirty_ring->size - 1)];
	entry->slot = slot;
	entry->offset = offset;
	smp_wmb();
	dirty_ring->dirty_index++;
	num = dirty_ring->dirty_index - dirty_ring->reset_index;
	WRITE_ONCE(vcpu->run->dirty_ring_indexes.avail_index,
		   dirty_ring->dirty_index % dirty_ring->size);
	ret = num >= dirty_ring->soft_limit;

out:
	spin_unlock(&dirty_ring->lock);
	return ret;
}

struct page *kvm_dirty_ring_get_page(struct kvm_vcpu *vcpu, u32 i)
{
	return vmalloc_to_page((void *)vcpu->dirty_ring.dirty_gfns
			       + i * PAGE_SIZE);
}

void kvm_dirty_ring_free(struct kvm_vcpu *vcpu)
{
	if (vcpu->dirty_ring.dirty_gfns) {
		vfree(vcpu->dirty_ring.dirty_gfns);
		vcpu->dirty_ring.dirty_gfns = NULL;
	}
}
