#include <linux/module.h>
#define INCLUDE_VERMAGIC
#include <linux/build-salt.h>
#include <linux/elfnote-lto.h>
#include <linux/vermagic.h>
#include <linux/compiler.h>

BUILD_SALT;
BUILD_LTO_INFO;

MODULE_INFO(vermagic, VERMAGIC_STRING);
MODULE_INFO(name, KBUILD_MODNAME);

__visible struct module __this_module
__section(".gnu.linkonce.this_module") = {
	.name = KBUILD_MODNAME,
	.init = init_module,
#ifdef CONFIG_MODULE_UNLOAD
	.exit = cleanup_module,
#endif
	.arch = MODULE_ARCH_INIT,
};

#ifdef CONFIG_RETPOLINE
MODULE_INFO(retpoline, "Y");
#endif

static const struct modversion_info ____versions[]
__used __section("__versions") = {
	{ 0x8f96b752, "module_layout" },
	{ 0xf513389e, "kobject_put" },
	{ 0x6bc3fbc0, "__unregister_chrdev" },
	{ 0x78c62afa, "d_path" },
	{ 0x33483032, "kernel_write" },
	{ 0xfbd9116b, "kmalloc_caches" },
	{ 0xeb233a45, "__kmalloc" },
	{ 0xf9a482f9, "msleep" },
	{ 0xd6ee688f, "vmalloc" },
	{ 0x27864d57, "memparse" },
	{ 0x91b9a4ba, "e820__mapped_any" },
	{ 0x77358855, "iomem_resource" },
	{ 0x754d539c, "strlen" },
	{ 0x3854774b, "kstrtoll" },
	{ 0xa4191c0b, "memset_io" },
	{ 0x142aba41, "pci_remove_root_bus" },
	{ 0xd36dc10c, "get_random_u32" },
	{ 0x837b7b09, "__dynamic_pr_debug" },
	{ 0x96ba676d, "filp_close" },
	{ 0xe091c977, "list_sort" },
	{ 0xeae3dfd6, "__const_udelay" },
	{ 0x3213f038, "mutex_unlock" },
	{ 0x85df9b6c, "strsep" },
	{ 0x999e8297, "vfree" },
	{ 0x7a2af7b4, "cpu_number" },
	{ 0x94e481a1, "path_get" },
	{ 0x97651e6c, "vmemmap_base" },
	{ 0x3c3ff9fd, "sprintf" },
	{ 0xebaf0133, "pv_ops" },
	{ 0x7706cbd4, "kthread_create_on_node" },
	{ 0xe2d5255a, "strcmp" },
	{ 0x95ce7032, "kobject_create_and_add" },
	{ 0x85628db2, "kthread_bind" },
	{ 0x7d4543a9, "kernel_read" },
	{ 0xfb578fc5, "memset" },
	{ 0x4c62fd7, "__memset" },
	{ 0xa033d747, "next_arg" },
	{ 0x62a4798, "current_task" },
	{ 0x64127b67, "bitmap_find_next_zero_area_off" },
	{ 0xecac8407, "__memcpy" },
	{ 0xbcab6ee6, "sscanf" },
	{ 0xfef216eb, "_raw_spin_trylock" },
	{ 0x887baeeb, "kthread_stop" },
	{ 0xe1537255, "__list_del_entry_valid" },
	{ 0x449ad0a7, "memcmp" },
	{ 0xeadaedee, "pci_scan_bus" },
	{ 0x693957b5, "dma_sync_wait" },
	{ 0x28045d4a, "__x86_indirect_alt_call_r15" },
	{ 0x9166fada, "strncpy" },
	{ 0x9e9fdd9d, "memunmap" },
	{ 0x4dfa8d4b, "mutex_lock" },
	{ 0xc26d456c, "pci_bus_add_devices" },
	{ 0x1d0053b0, "irq_get_irq_data" },
	{ 0xce8b1878, "__x86_indirect_thunk_r14" },
	{ 0x8c8569cb, "kstrtoint" },
	{ 0x68f31cbd, "__list_add_valid" },
	{ 0x21445520, "pci_stop_root_bus" },
	{ 0x800473f, "__cond_resched" },
	{ 0x542be051, "__x86_indirect_alt_jmp_rax" },
	{ 0x7cd8d75e, "page_offset_base" },
	{ 0x40a9b349, "vzalloc" },
	{ 0x618911fc, "numa_node" },
	{ 0xa8394a7d, "sysfs_remove_file_ns" },
	{ 0xd0da656b, "__stack_chk_fail" },
	{ 0xb8b9f817, "kmalloc_order_trace" },
	{ 0x92997ed8, "_printk" },
	{ 0x2ea2c95c, "__x86_indirect_thunk_rax" },
	{ 0xec619403, "wake_up_process" },
	{ 0xbdfb6dbb, "__fentry__" },
	{ 0x99cb93b0, "__dma_request_channel" },
	{ 0xcbd4898c, "fortify_panic" },
	{ 0x385aadde, "kmem_cache_alloc_trace" },
	{ 0xba8fbd64, "_raw_spin_lock" },
	{ 0xb19a5453, "__per_cpu_offset" },
	{ 0x4d924f20, "memremap" },
	{ 0xb3f7646e, "kthread_should_stop" },
	{ 0x1fd7de6f, "dma_release_channel" },
	{ 0x37a0cba, "kfree" },
	{ 0x69acdf38, "memcpy" },
	{ 0x9e61bb05, "set_freezable" },
	{ 0xe98a828a, "__x86_indirect_alt_call_r14" },
	{ 0xf05c7b8, "__x86_indirect_thunk_r15" },
	{ 0x9a353ae, "__x86_indirect_alt_call_rax" },
	{ 0xb742fd7, "simple_strtol" },
	{ 0x656e4a6e, "snprintf" },
	{ 0x77bc13a0, "strim" },
	{ 0xae1421fa, "sysfs_create_file_ns" },
	{ 0xceb66bec, "sched_clock_cpu" },
	{ 0xa2806cfd, "filp_open" },
};

MODULE_INFO(depends, "");

