#include "param.h"
#include "types.h"
#include "defs.h"
#include "x86.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "elf.h"

extern char data[];  // defined by kernel.ld
pde_t *kpgdir;  // for use in scheduler()

void swap_out(void* virtual_address, struct proc* proc);
int findNextFreeIndex(void** arr, struct proc* proc);
void* find_page_to_swap(struct proc* proc);
int find_page_index(void* p, struct proc* proc);
void insert_page(void* virtual_address, struct proc* proc);
void update_page(void *virtual_address, int physical_index, struct proc* proc);

// Set up CPU's kernel segment descriptors.
// Run once on entry on each CPU.
void
seginit(void)
{
  struct cpu *c;

  // Map "logical" addresses to virtual addresses using identity map.
  // Cannot share a CODE descriptor for both kernel and user
  // because it would have to have DPL_USR, but the CPU forbids
  // an interrupt from CPL=0 to DPL=3.
  c = &cpus[cpuid()];
  c->gdt[SEG_KCODE] = SEG(STA_X|STA_R, 0, 0xffffffff, 0);
  c->gdt[SEG_KDATA] = SEG(STA_W, 0, 0xffffffff, 0);
  c->gdt[SEG_UCODE] = SEG(STA_X|STA_R, 0, 0xffffffff, DPL_USER);
  c->gdt[SEG_UDATA] = SEG(STA_W, 0, 0xffffffff, DPL_USER);
  lgdt(c->gdt, sizeof(c->gdt));
}

// Return the address of the PTE in page table pgdir
// that corresponds to virtual address va.  If alloc!=0,
// create any required page table pages.
static pte_t *
walkpgdir(pde_t *pgdir, const void *va, int alloc)
{
  pde_t *pde;
  pte_t *pgtab;

  pde = &pgdir[PDX(va)];
  if(*pde & PTE_P){
    pgtab = (pte_t*)P2V(PTE_ADDR(*pde));
  } else {
    if(!alloc || (pgtab = (pte_t*)kalloc()) == 0)
      return 0;
    // Make sure all those PTE_P bits are zero.
    memset(pgtab, 0, PGSIZE);
    // The permissions here are overly generous, but they can
    // be further restricted by the permissions in the page table
    // entries, if necessary.
    *pde = V2P(pgtab) | PTE_P | PTE_W | PTE_U;
  }
  return &pgtab[PTX(va)];
}

// Create PTEs for virtual addresses starting at va that refer to
// physical addresses starting at pa. va and size might not
// be page-aligned.
static int
mappages(pde_t *pgdir, void *va, uint size, uint pa, int perm)
{
  char *a, *last;
  pte_t *pte;

  a = (char*)PGROUNDDOWN((uint)va);
  last = (char*)PGROUNDDOWN(((uint)va) + size - 1);
  for(;;){
    if((pte = walkpgdir(pgdir, a, 1)) == 0)
      return -1;
    if(*pte & PTE_P)
      panic("remap");
    *pte = pa | perm | PTE_P;
    if(a == last)
      break;
    a += PGSIZE;
    pa += PGSIZE;
  }
  return 0;
}

// There is one page table per process, plus one that's used when
// a CPU is not running any process (kpgdir). The kernel uses the
// current process's page table during system calls and interrupts;
// page protection bits prevent user code from using the kernel's
// mappings.
//
// setupkvm() and exec() set up every page table like this:
//
//   0..KERNBASE: user memory (text+data+stack+heap), mapped to
//                phys memory allocated by the kernel
//   KERNBASE..KERNBASE+EXTMEM: mapped to 0..EXTMEM (for I/O space)
//   KERNBASE+EXTMEM..data: mapped to EXTMEM..V2P(data)
//                for the kernel's instructions and r/o data
//   data..KERNBASE+PHYSTOP: mapped to V2P(data)..PHYSTOP,
//                                  rw data + free physical memory
//   0xfe000000..0: mapped direct (devices such as ioapic)
//
// The kernel allocates physical memory for its heap and for user memory
// between V2P(end) and the end of physical memory (PHYSTOP)
// (directly addressable from end..P2V(PHYSTOP)).

// This table defines the kernel's mappings, which are present in
// every process's page table.
static struct kmap {
  void *virt;
  uint phys_start;
  uint phys_end;
  int perm;
} kmap[] = {
 { (void*)KERNBASE, 0,             EXTMEM,    PTE_W}, // I/O space
 { (void*)KERNLINK, V2P(KERNLINK), V2P(data), 0},     // kern text+rodata
 { (void*)data,     V2P(data),     PHYSTOP,   PTE_W}, // kern data+memory
 { (void*)DEVSPACE, DEVSPACE,      0,         PTE_W}, // more devices
};

// Set up kernel part of a page table.
pde_t*
setupkvm(void)
{
  pde_t *pgdir;
  struct kmap *k;

  if((pgdir = (pde_t*)kalloc()) == 0)
    return 0;
  memset(pgdir, 0, PGSIZE);
  if (P2V(PHYSTOP) > (void*)DEVSPACE)
    panic("PHYSTOP too high");
  for(k = kmap; k < &kmap[NELEM(kmap)]; k++)
    if(mappages(pgdir, k->virt, k->phys_end - k->phys_start,
                (uint)k->phys_start, k->perm) < 0) {
      freevm(pgdir, myproc());
      return 0;
    }
  return pgdir;
}

// Allocate one page table for the machine for the kernel address
// space for scheduler processes.
void
kvmalloc(void)
{
  kpgdir = setupkvm();
  switchkvm();
}

// Switch h/w page table register to the kernel-only page table,
// for when no process is running.
void
switchkvm(void)
{
  lcr3(V2P(kpgdir));   // switch to the kernel page table
}

// Switch TSS and h/w page table to correspond to process p.
void
switchuvm(struct proc *p)
{
  if(p == 0)
    panic("switchuvm: no process");
  if(p->kstack == 0)
    panic("switchuvm: no kstack");
  if(p->pgdir == 0)
    panic("switchuvm: no pgdir");

  pushcli();
  mycpu()->gdt[SEG_TSS] = SEG16(STS_T32A, &mycpu()->ts,
                                sizeof(mycpu()->ts)-1, 0);
  mycpu()->gdt[SEG_TSS].s = 0;
  mycpu()->ts.ss0 = SEG_KDATA << 3;
  mycpu()->ts.esp0 = (uint)p->kstack + KSTACKSIZE;
  // setting IOPL=0 in eflags *and* iomb beyond the tss segment limit
  // forbids I/O instructions (e.g., inb and outb) from user space
  mycpu()->ts.iomb = (ushort) 0xFFFF;
  ltr(SEG_TSS << 3);
  lcr3(V2P(p->pgdir));  // switch to process's address space
  popcli();
}

// Load the initcode into address 0 of pgdir.
// sz must be less than a page.
void
inituvm(pde_t *pgdir, char *init, uint sz)
{
  char *mem;

  if(sz >= PGSIZE)
    panic("inituvm: more than a page");
  mem = kalloc();
  memset(mem, 0, PGSIZE);
  mappages(pgdir, 0, PGSIZE, V2P(mem), PTE_W|PTE_U);
  memmove(mem, init, sz);
}

// Load a program segment into pgdir.  addr must be page-aligned
// and the pages from addr to addr+sz must already be mapped.
int
loaduvm(pde_t *pgdir, char *addr, struct inode *ip, uint offset, uint sz)
{
  uint i, pa, n;
  pte_t *pte;

  if((uint) addr % PGSIZE != 0)
    panic("loaduvm: addr must be page aligned");
  for(i = 0; i < sz; i += PGSIZE){
    if((pte = walkpgdir(pgdir, addr+i, 0)) == 0)
      panic("loaduvm: address should exist");
    pa = PTE_ADDR(*pte);
    if(sz - i < PGSIZE)
      n = sz - i;
    else
      n = PGSIZE;
    if(readi(ip, P2V(pa), offset+i, n) != n)
      return -1;
  }
  return 0;
}

// Allocate page tables and physical memory to grow process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
int
allocuvm(pde_t *pgdir, uint oldsz, uint newsz)
{
  char *mem;
  uint a;
  struct proc* proc = myproc();
  if(newsz >= KERNBASE)
    return 0;
  if(newsz < oldsz)
    return oldsz;

  a = PGROUNDUP(oldsz);
  for(; a < newsz; a += PGSIZE){
	#ifndef NONE
    if(proc->pid > 2 && proc->is_alocated) {
      insert_page((void*)a, proc);
    }
    #endif
	  
    mem = kalloc();
    if(mem == 0){
      cprintf("allocuvm out of memory\n");
      deallocuvm(pgdir, newsz, oldsz, proc);
      return 0;
    }
    memset(mem, 0, PGSIZE);
    if(mappages(pgdir, (char*)a, PGSIZE, V2P(mem), PTE_W|PTE_U) < 0){
      cprintf("allocuvm out of memory (2)\n");
      deallocuvm(pgdir, newsz, oldsz, proc);
      kfree(mem);
      return 0;
    }
  }
  return newsz;
}


void
deallocPageFromProc(struct proc* p, uint a){
  int physical_index = find_page_index((void*)PTE_ADDR(a), p);
  if(physical_index == -1) // if not found on proc arrays of pages
    return;
  p->swapped_in_count--; 
  p->swapped_in[physical_index].virtual_address = (void*) -1;
}

// Deallocate user pages to bring the process size from oldsz to
// newsz.  oldsz and newsz need not be page-aligned, nor does newsz
// need to be less than oldsz.  oldsz can be larger than the actual
// process size.  Returns the new process size.
int
deallocuvm(pde_t *pgdir, uint oldsz, uint newsz, struct proc* np)
{
  pte_t *pte;
  uint a, pa;

  if(newsz >= oldsz)
    return oldsz;

  a = PGROUNDUP(newsz);
  for(; a  < oldsz; a += PGSIZE){
    pte = walkpgdir(pgdir, (char*)a, 0);
    if(!pte)
      a = PGADDR(PDX(a) + 1, 0, 0) - PGSIZE;
    else if((*pte & PTE_P) != 0){
		#ifndef NONE
      if (np && np->pid >2 && np->is_alocated ) {
          deallocPageFromProc(np, a);    
      }
    #endif
		
      pa = PTE_ADDR(*pte);
      if(pa == 0)
        panic("kfree");
      char *v = P2V(pa);
      kfree(v);
      *pte = 0;
    }
  }
  return newsz;
}

// Free a page table and all the physical memory pages
// in the user part.
void
freevm(pde_t *pgdir, struct proc *np)
{
  uint i;

  if(pgdir == 0)
    panic("freevm: no pgdir");
  deallocuvm(pgdir, KERNBASE, 0, np);
  for(i = 0; i < NPDENTRIES; i++){
    if(pgdir[i] & PTE_P){
      char * v = P2V(PTE_ADDR(pgdir[i]));
      kfree(v);
    }
  }
  kfree((char*)pgdir);
}

// Clear PTE_U on a page. Used to create an inaccessible
// page beneath the user stack.
void
clearpteu(pde_t *pgdir, char *uva)
{
  pte_t *pte;

  pte = walkpgdir(pgdir, uva, 0);
  if(pte == 0)
    panic("clearpteu");
  *pte &= ~PTE_U;
}

// Given a parent process's page table, create a copy
// of it for a child.
pde_t*
copyuvm(pde_t *pgdir, uint sz)
{
  pde_t *d;
  pte_t *pte;
  uint pa, i, flags;
  char *mem;

  if((d = setupkvm()) == 0)
    return 0;
  for(i = 0; i < sz; i += PGSIZE){
    if((pte = walkpgdir(pgdir, (void *) i, 0)) == 0)
      panic("copyuvm: pte should exist");
    if ((*pte & PTE_PG) != 0) {
      mappages(d, (void*)i, PGSIZE, 0, PTE_FLAGS(*pte));
      pte_t* pte = walkpgdir(d, (void*)i, 0);
      *pte &= (~PTE_P);
      continue;
    }
  
    if(!(*pte & PTE_P))
      panic("copyuvm: page not present");
    pa = PTE_ADDR(*pte);
    flags = PTE_FLAGS(*pte);
    if((mem = kalloc()) == 0)
      goto bad;
    memmove(mem, (char*)P2V(pa), PGSIZE);
    if(mappages(d, (void*)i, PGSIZE, V2P(mem), flags) < 0)
      goto bad;
  }
  return d;

bad:
  freevm(d,myproc());
  return 0;
}

//PAGEBREAK!
// Map user virtual address to kernel address.
char*
uva2ka(pde_t *pgdir, char *uva)
{
  pte_t *pte;

  pte = walkpgdir(pgdir, uva, 0);
  if((*pte & PTE_P) == 0)
    return 0;
  if((*pte & PTE_U) == 0)
    return 0;
  return (char*)P2V(PTE_ADDR(*pte));
}

// Copy len bytes from p to user address va in page table pgdir.
// Most useful when pgdir is not the current page table.
// uva2ka ensures this only works for PTE_U pages.
int
copyout(pde_t *pgdir, uint va, void *p, uint len)
{
  char *buf, *pa0;
  uint n, va0;

  buf = (char*)p;
  while(len > 0){
    va0 = (uint)PGROUNDDOWN(va);
    pa0 = uva2ka(pgdir, (char*)va0);
    if(pa0 == 0)
      return -1;
    n = PGSIZE - (va - va0);
    if(n > len)
      n = len;
    memmove(pa0 + (va - va0), buf, n);
    len -= n;
    buf += n;
    va = va0 + PGSIZE;
  }
  return 0;
}

//PAGEBREAK!
// Blank page.
//PAGEBREAK!
// Blank page.
//PAGEBREAK!
// Blank page.


void
shift_physical_addresses_left(int start, struct proc* proc){
  for (int i = start; i < MAX_PHYS_PAGES; ++i)
    proc->swapped_in[i - 1] = proc->swapped_in[i];
}

void
shift_physical_addresses_right(struct proc* proc){
  for (int i = MAX_PHYS_PAGES - 1; i >0; i--)
    proc->swapped_in[i] = proc->swapped_in[i-1];
}

int
find_free_virtual_index(struct proc* proc){
  for (int i = 0; i < MAX_TOTAL_PAGES - MAX_PHYS_PAGES; ++i)
  {
    if(proc->swapped_out[i] == 0){
      return i;
    }
  }
  return -1;
}

int
find_free_physical_index(struct proc* proc){
  #ifndef AQ
  for (int i = 0; i < MAX_PHYS_PAGES; ++i)
  {
    if(proc->swapped_in[i].virtual_address == (void*) -1){
      return i;
    }
  }
  panic("find_free_physical_index");
  #endif
  
  #ifdef AQ
  if(proc->swapped_in[MAX_PHYS_PAGES-1].virtual_address != (void*) -1){
	  panic("find_free_physical_index");
  }
  shift_physical_addresses_right(proc);
  return 0;
  #endif
}

int
find_virtual_index(void* p, struct proc* proc){
  for (int i = 0; i < MAX_TOTAL_PAGES - MAX_PHYS_PAGES; ++i)
  {
    if(proc->swapped_out[i] == p){
      return i;
    }
  }
  return -1;
}

int
find_page_index(void* p, struct proc* proc){
  for (int i = 0; i < MAX_PHYS_PAGES; ++i)
  {
    if(proc->swapped_in[i].virtual_address == p){
      return i;
    }
  }
  return -1;
}

void*
handle_NFUA(struct proc* proc){
  int min_index = 0;
  int min_accessed = proc->swapped_in[min_index].access_count;
  for (int i = 0; i < MAX_PHYS_PAGES; ++i)
  {
    if(proc->swapped_in[i].access_count < min_accessed){
      min_index = i;
      min_accessed = proc->swapped_in[i].access_count;
    }
  }
  
  //cprintf("Address returned from handle_NFUA is: %x, at index %d\n ",virtual_address, min_index);
  return proc->swapped_in[min_index].virtual_address;
}

int
count_ones(uint n){
	int count = 0;
	while(n > 0){
		count += n & 1;
		n >>= 1;
	}
	return count;
}

void*
handle_LAPA(struct proc* proc){
  int ones_counts[MAX_PHYS_PAGES];
  for (int i = 0; i < MAX_PHYS_PAGES; ++i){
	  ones_counts[i] = count_ones(proc->swapped_in[i].access_count);
    //cprintf("ones count %d: %d    access count: %d\n", i, ones_counts[i], proc->swapped_in[i].access_count);
  }
	  
  int min_index = 0;
  int current, min_ones = ones_counts[min_index];
  for (int i = 0; i < MAX_PHYS_PAGES; i++)
  {
	current = ones_counts[i];
    if(current < min_ones){
      min_index = i;
      min_ones = current;
    }
    else if(current == min_ones){
		  if(proc->swapped_in[i].access_count < proc->swapped_in[min_index].access_count){
			  min_index = i;
		  }
	  }
  }
  //cprintf("selected = %d", min_index);
  return proc->swapped_in[min_index].virtual_address;
}



void*
handle_SCFIFO(struct proc* proc){
  void* virtual_address = (void*) -1;
  
  uint intena = 1;
  while(intena)
  {
    pte_t* pte = walkpgdir(proc->pgdir, (void*)PTE_ADDR(proc->swapped_in[0].virtual_address), 0);
    if(*pte & PTE_A){
      *pte = *pte & (~PTE_A);
      struct page temp = proc->swapped_in[0];
      shift_physical_addresses_left(1, proc);
      proc->swapped_in[MAX_PHYS_PAGES - 1] = temp;
    }
    else{
      virtual_address = proc->swapped_in[0].virtual_address;
      shift_physical_addresses_left(1, proc);
      proc->swapped_in[MAX_PHYS_PAGES - 1].virtual_address = virtual_address;
      break;
    }
  }
  return virtual_address;
}

void*
handle_AQ(struct proc* proc){
	return proc->swapped_in[MAX_PHYS_PAGES - 1].virtual_address;
}

void*
find_page_to_swap(struct proc* proc){
#ifdef NFUA
    return handle_NFUA(proc);
#elif LAPA
    return handle_LAPA(proc);
#elif SCFIFO
    return handle_SCFIFO(proc);
#elif AQ
    return handle_AQ(proc);
#endif 
  return 0;
}

void 
swap_out(void* virtual_address, struct proc* proc) {
  pte_t* pte = walkpgdir(proc->pgdir, (void*)PTE_ADDR(virtual_address), 0);
  //cprintf("swap_out got %x as virtual_address\n", virtual_address);
  
  if (pte == 0) 
    panic("swap_out : null entry");

  *pte &= (~PTE_P);
  *pte |= (PTE_PG);

  int page_index = find_free_virtual_index(proc);
  //cprintf("	swap_out got page index %d\n ", page_index);
  if(page_index == -1)
    panic("swap_out : file is full");
  proc->swapped_out[page_index] = (void*)PTE_ADDR(virtual_address);
  proc->total_swapped_out_count++;
  proc->swapped_out_count++;
  
  uint file_offset = page_index * PGSIZE ;

  writeToSwapFile(proc, (char*)PTE_ADDR(P2V(*pte)), file_offset, PGSIZE);

  int physical_index = find_page_index((void*)PTE_ADDR(virtual_address), proc);
  //cprintf("	swap_out physical_index index %d \n", physical_index);
  proc->swapped_in[physical_index].virtual_address = (void*) -1;
  proc->swapped_in_count--;
  
  lcr3(V2P(proc->pgdir)); 
  
  kfree((char*)PTE_ADDR(P2V(*pte)));
}

int 
swap_in(void* virtual_address, struct proc* proc) {
  pte_t* pte = walkpgdir(proc->pgdir, (char*)PTE_ADDR(virtual_address), 0);

  if (pte == 0) 
    panic("swap_in : entry is null");

  if(!(*pte & PTE_P) && (*pte & PTE_PG)){
    *pte &= (~PTE_A);
    int page_index = find_virtual_index((void*)PTE_ADDR(virtual_address),proc);
    if(page_index == -1){
      panic("swap_in : page not found");
    }
    
    if(proc->swapped_in_count >= MAX_PHYS_PAGES){
	  //cprintf("swap in\n");
      swap_out(find_page_to_swap(proc), proc);
    }

    char* page_address = kalloc();
    if (readFromSwapFile(proc, page_address, page_index * PGSIZE, PGSIZE) == -1){
      panic("swap_in : error while reading");
	}

    mappages(proc->pgdir, (char*)PTE_ADDR(virtual_address), PGSIZE, V2P(page_address), PTE_W | PTE_U);
	
    proc->swapped_out[page_index] = 0;
	proc->swapped_out_count--;
	
    int physical_index = find_free_physical_index(proc);
    update_page(virtual_address, physical_index, proc);
	proc->swapped_in_count++;

    return 1;
  }

  return 0;
}

void
insert_page(void* virtual_address, struct proc* proc){
  pte_t* pte = walkpgdir(proc->pgdir, (char*)virtual_address, 0);
  
  if (pte == 0) {
    panic("insert_page");
  }
  *pte &= (~PTE_A);
  if(proc->swapped_in_count >= MAX_PHYS_PAGES){
	//cprintf("insert_page before call to swap out \n");
    swap_out(find_page_to_swap(proc), proc);
	//cprintf("insert_page after to swap out \n");
  }
  //cprintf("insert_page before call to find_free_physical_index \n");
  int physical_index = find_free_physical_index(proc);
  //cprintf("insert_page after call to find_free_physical_index\n ");
  //cprintf("insert_page before call to update_page\n ");
  update_page(virtual_address, physical_index, proc);
  //cprintf("insert_page after call to update_page\n ");
  proc->swapped_in_count++;
}

void
update_page(void *virtual_address, int physical_index, struct proc* proc){
  proc->swapped_in[physical_index].virtual_address = (void*)(PTE_ADDR(virtual_address));
  #ifdef NFUA
  proc->swapped_in[physical_index].access_count = 0;
  #elif LAPA
  proc->swapped_in[physical_index].access_count = 0xFFFFFFFF;
  #endif
}

void
update_process_pages_access(struct proc* p){
  #ifdef AQ
  for (int i = 0; i < MAX_PHYS_PAGES - 1 ; ++i)
  {
    pte_t* current = walkpgdir(p->pgdir, (void*)PTE_ADDR(p->swapped_in[i].virtual_address), 0);
	  pte_t* prev_in_q = walkpgdir(p->pgdir, (void*)PTE_ADDR(p->swapped_in[i+1].virtual_address), 0);
    if(!(*current & PTE_A) && (*prev_in_q & PTE_A)){
      struct page temp = p->swapped_in[i+1];
      p->swapped_in[i+1] = p->swapped_in[i];
      p->swapped_in[i] = temp;
    }
  }
  #endif
  #ifndef SCFIFO
  for (int i = 0; i < MAX_PHYS_PAGES; ++i)
  {
    pte_t* pte = walkpgdir(p->pgdir, (void*)PTE_ADDR(p->swapped_in[i].virtual_address), 0);
	  p->swapped_in[i].access_count >>= 1;
    if(*pte & PTE_A){
      *pte = *pte & (~PTE_A);
	    p->swapped_in[i].access_count |= (1 << ((sizeof(int) * 8) - 1));
    }
  }
  #endif
}