/* fhandler_fifo.cc - See fhandler.h for a description of the fhandler classes.

   This file is part of Cygwin.

   This software is a copyrighted work licensed under the terms of the
   Cygwin license.  Please consult the file "CYGWIN_LICENSE" for
   details. */

#include "winsup.h"
#include <w32api/winioctl.h>
#include "miscfuncs.h"

#include "cygerrno.h"
#include "security.h"
#include "path.h"
#include "fhandler.h"
#include "dtable.h"
#include "cygheap.h"
#include "sigproc.h"
#include "cygtls.h"
#include "shared_info.h"
#include "ntdll.h"
#include "cygwait.h"
#include <sys/param.h>

/*
   Overview:

     Currently a FIFO can be opened once for reading and multiple
     times for writing.  Any attempt to open the FIFO a second time
     for reading fails with EACCES (from STATUS_ACCESS_DENIED).

     When a FIFO is opened for reading,
     fhandler_fifo::create_pipe_instance is called to create the first
     instance of a Windows named pipe server (Windows terminology).  A
     "fifo_reader" thread is also started; it waits for pipe clients
     (Windows terminology again) to connect.  This happens every time
     a process opens the FIFO for writing.

     The fifo_reader thread creates new instances of the pipe server
     as needed, so that there is always an instance available for a
     writer to connect to.

     The reader maintains a list of "fifo_client_handlers", one for
     each pipe instance.  A fifo_client_handler manages the connection
     between the pipe instance and a writer connected to that pipe
     instance.

     TODO: Allow a FIFO to be opened multiple times for reading.
     Maybe this could be done by using shared memory, so that all
     readers could have access to the same list of writers.
*/


/* This is only to be used for writers.  When reading,
STATUS_PIPE_EMPTY simply means there's no data to be read. */
#define STATUS_PIPE_IS_CLOSED(status)	\
		({ NTSTATUS _s = (status); \
		   _s == STATUS_PIPE_CLOSING \
		   || _s == STATUS_PIPE_BROKEN \
		   || _s == STATUS_PIPE_EMPTY; })

#define STATUS_PIPE_NO_INSTANCE_AVAILABLE(status)	\
		({ NTSTATUS _s = (status); \
		   _s == STATUS_INSTANCE_NOT_AVAILABLE \
		   || _s == STATUS_PIPE_NOT_AVAILABLE \
		   || _s == STATUS_PIPE_BUSY; })

/* Number of pages reserved for shared_fc_handler. */
#define SH_FC_HANDLER_PAGES 100

static NO_COPY fifo_reader_id_t null_fr_id = { .winpid = 0, .fh = NULL };

fhandler_fifo::fhandler_fifo ():
  fhandler_base (),
  read_ready (NULL), write_ready (NULL), writer_opening (NULL),
  cancel_evt (NULL), thr_sync_evt (NULL), _maybe_eof (false),
  fc_handler (NULL), shandlers (0), nhandlers (0),
  reader (false), writer (false), duplexer (false),
  max_atomic_write (DEFAULT_PIPEBUFSIZE),
  me (null_fr_id), shmem_handle (NULL), shmem (NULL),
  shared_fc_hdl (NULL), shared_fc_handler (NULL)
{
  pipe_name_buf[0] = L'\0';
  need_fork_fixup (true);
}

PUNICODE_STRING
fhandler_fifo::get_pipe_name ()
{
  if (!pipe_name_buf[0])
    {
      __small_swprintf (pipe_name_buf, L"%S-fifo.%08x.%016X",
			&cygheap->installation_key, get_dev (), get_ino ());
      RtlInitUnicodeString (&pipe_name, pipe_name_buf);
    }
  return &pipe_name;
}

inline PSECURITY_ATTRIBUTES
sec_user_cloexec (bool cloexec, PSECURITY_ATTRIBUTES sa, PSID sid)
{
  return cloexec ? sec_user_nih (sa, sid) : sec_user (sa, sid);
}

static HANDLE
create_event (bool inherit = false)
{
  NTSTATUS status;
  OBJECT_ATTRIBUTES attr;
  HANDLE evt = NULL;

  InitializeObjectAttributes (&attr, NULL, inherit ? OBJ_INHERIT : 0,
			      NULL, NULL);
  status = NtCreateEvent (&evt, EVENT_ALL_ACCESS, &attr,
			  NotificationEvent, FALSE);
  if (!NT_SUCCESS (status))
    __seterrno_from_nt_status (status);
  return evt;
}


static void
set_pipe_non_blocking (HANDLE ph, bool nonblocking)
{
  NTSTATUS status;
  IO_STATUS_BLOCK io;
  FILE_PIPE_INFORMATION fpi;

  fpi.ReadMode = FILE_PIPE_MESSAGE_MODE;
  fpi.CompletionMode = nonblocking ? FILE_PIPE_COMPLETE_OPERATION
    : FILE_PIPE_QUEUE_OPERATION;
  status = NtSetInformationFile (ph, &io, &fpi, sizeof fpi,
				 FilePipeInformation);
  if (!NT_SUCCESS (status))
    debug_printf ("NtSetInformationFile(FilePipeInformation): %y", status);
}

NTSTATUS
fhandler_fifo::npfs_handle (HANDLE &nph)
{
  static NO_COPY SRWLOCK npfs_lock;
  static NO_COPY HANDLE npfs_dirh;

  NTSTATUS status = STATUS_SUCCESS;
  OBJECT_ATTRIBUTES attr;
  IO_STATUS_BLOCK io;

  /* Lockless after first call. */
  if (npfs_dirh)
    {
      nph = npfs_dirh;
      return STATUS_SUCCESS;
    }
  AcquireSRWLockExclusive (&npfs_lock);
  if (!npfs_dirh)
    {
      InitializeObjectAttributes (&attr, &ro_u_npfs, 0, NULL, NULL);
      status = NtOpenFile (&npfs_dirh, FILE_READ_ATTRIBUTES | SYNCHRONIZE,
			   &attr, &io, FILE_SHARE_READ | FILE_SHARE_WRITE,
			   0);
    }
  ReleaseSRWLockExclusive (&npfs_lock);
  if (NT_SUCCESS (status))
    nph = npfs_dirh;
  return status;
}

/* Called when a FIFO is first opened for reading and again each time
   a new client handler is needed.  Each pipe instance is created in
   blocking mode so that we can easily wait for a connection.  After
   it is connected, it is put in nonblocking mode. */
HANDLE
fhandler_fifo::create_pipe_instance ()
{
  NTSTATUS status;
  HANDLE npfsh;
  HANDLE ph = NULL;
  ACCESS_MASK access;
  OBJECT_ATTRIBUTES attr;
  IO_STATUS_BLOCK io;
  ULONG hattr;
  ULONG sharing;
  ULONG nonblocking = FILE_PIPE_QUEUE_OPERATION;
  ULONG max_instances = -1;
  LARGE_INTEGER timeout;

  status = npfs_handle (npfsh);
  if (!NT_SUCCESS (status))
    {
      __seterrno_from_nt_status (status);
      return NULL;
    }
  access = GENERIC_READ | FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES
    | SYNCHRONIZE;
  sharing = FILE_SHARE_READ | FILE_SHARE_WRITE;
  hattr = (openflags & O_CLOEXEC ? 0 : OBJ_INHERIT) | OBJ_CASE_INSENSITIVE;
  InitializeObjectAttributes (&attr, get_pipe_name (),
			      hattr, npfsh, NULL);
  timeout.QuadPart = -500000;
  status = NtCreateNamedPipeFile (&ph, access, &attr, &io, sharing,
				  FILE_OPEN_IF, 0,
				  FILE_PIPE_MESSAGE_TYPE
				    | FILE_PIPE_REJECT_REMOTE_CLIENTS,
				  FILE_PIPE_MESSAGE_MODE,
				  nonblocking, max_instances,
				  DEFAULT_PIPEBUFSIZE, DEFAULT_PIPEBUFSIZE,
				  &timeout);
  if (!NT_SUCCESS (status))
    __seterrno_from_nt_status (status);
  return ph;
}

/* Connect to a pipe instance. */
NTSTATUS
fhandler_fifo::open_pipe (HANDLE& ph)
{
  NTSTATUS status;
  HANDLE npfsh;
  ACCESS_MASK access;
  OBJECT_ATTRIBUTES attr;
  IO_STATUS_BLOCK io;
  ULONG sharing;

  status = npfs_handle (npfsh);
  if (!NT_SUCCESS (status))
    return status;
  access = GENERIC_WRITE | SYNCHRONIZE;
  InitializeObjectAttributes (&attr, get_pipe_name (),
			      openflags & O_CLOEXEC ? 0 : OBJ_INHERIT,
			      npfsh, NULL);
  sharing = FILE_SHARE_READ | FILE_SHARE_WRITE;
  return NtOpenFile (&ph, access, &attr, &io, sharing, 0);
}

/* Wait up to 100ms for a pipe instance to be available, then connect. */
NTSTATUS
fhandler_fifo::wait_open_pipe (HANDLE& ph)
{
  HANDLE npfsh;
  HANDLE evt;
  NTSTATUS status;
  IO_STATUS_BLOCK io;
  ULONG pwbuf_size;
  PFILE_PIPE_WAIT_FOR_BUFFER pwbuf;
  LONGLONG stamp;
  LONGLONG orig_timeout = -100 * NS100PERSEC / MSPERSEC;   /* 100ms */

  status = npfs_handle (npfsh);
  if (!NT_SUCCESS (status))
    return status;
  if (!(evt = create_event ()))
    api_fatal ("Can't create event, %E");
  pwbuf_size
    = offsetof (FILE_PIPE_WAIT_FOR_BUFFER, Name) + get_pipe_name ()->Length;
  pwbuf = (PFILE_PIPE_WAIT_FOR_BUFFER) alloca (pwbuf_size);
  pwbuf->Timeout.QuadPart = orig_timeout;
  pwbuf->NameLength = get_pipe_name ()->Length;
  pwbuf->TimeoutSpecified = TRUE;
  memcpy (pwbuf->Name, get_pipe_name ()->Buffer, get_pipe_name ()->Length);
  stamp = get_clock (CLOCK_MONOTONIC)->n100secs ();
  bool retry;
  do
    {
      retry = false;
      status = NtFsControlFile (npfsh, evt, NULL, NULL, &io, FSCTL_PIPE_WAIT,
				pwbuf, pwbuf_size, NULL, 0);
      if (status == STATUS_PENDING)
	{
	  if (WaitForSingleObject (evt, INFINITE) == WAIT_OBJECT_0)
	    status = io.Status;
	  else
	    api_fatal ("WFSO failed, %E");
	}
      if (NT_SUCCESS (status))
	status = open_pipe (ph);
      if (STATUS_PIPE_NO_INSTANCE_AVAILABLE (status))
	{
	  /* Another writer has grabbed the pipe instance.  Adjust
	     the timeout and keep waiting if there's time left. */
	  pwbuf->Timeout.QuadPart = orig_timeout
	    + get_clock (CLOCK_MONOTONIC)->n100secs () - stamp;
	  if (pwbuf->Timeout.QuadPart < 0)
	    retry = true;
	  else
	    status = STATUS_IO_TIMEOUT;
	}
    }
  while (retry);
  NtClose (evt);
  return status;
}

int
fhandler_fifo::add_client_handler (bool new_pipe_instance)
{
  fifo_client_handler fc;

  if (nhandlers >= shandlers)
    {
      void *temp = realloc (fc_handler,
			    (shandlers += 64) * sizeof (fc_handler[0]));
      if (!temp)
	{
	  shandlers -= 64;
	  set_errno (ENOMEM);
	  return -1;
	}
      fc_handler = (fifo_client_handler *) temp;
    }
  if (new_pipe_instance)
    {
      HANDLE ph = create_pipe_instance ();
      if (!ph)
	return -1;
      fc.h = ph;
      fc.state = fc_listening;
    }
  fc_handler[nhandlers++] = fc;
  return 0;
}

void
fhandler_fifo::delete_client_handler (int i)
{
  fc_handler[i].close ();
  if (i < --nhandlers)
    memmove (fc_handler + i, fc_handler + i + 1,
	     (nhandlers - i) * sizeof (fc_handler[i]));
}

/* Delete invalid handlers. */
void
fhandler_fifo::cleanup_handlers ()
{
  int i = 0;

  while (i < nhandlers)
    {
      if (fc_handler[i].state < fc_connected)
	delete_client_handler (i);
      else
	i++;
    }
}

void
fhandler_fifo::record_connection (fifo_client_handler& fc,
				  fifo_client_connect_state s)
{
  fc.state = s;
  maybe_eof (false);
  ResetEvent (writer_opening);
  set_pipe_non_blocking (fc.h, true);
}

/* Called from fifo_reader_thread_func with owner_lock in place, also
   from fixup_after_exec with shared handles useable as they are. */
int
fhandler_fifo::update_my_handlers (bool from_exec)
{
  if (from_exec)
    {
      nhandlers = get_shared_nhandlers ();
      if (nhandlers > shandlers)
	{
	  int save = shandlers;
	  shandlers = nhandlers + 64;
	  void *temp = realloc (fc_handler,
				shandlers * sizeof (fc_handler[0]));
	  if (!temp)
	    {
	      shandlers = save;
	      nhandlers = 0;
	      set_errno (ENOMEM);
	      return -1;
	    }
	  fc_handler = (fifo_client_handler *) temp;
	}
      memcpy (fc_handler, shared_fc_handler,
	      nhandlers * sizeof (fc_handler[0]));
    }
  else
    {
      close_all_handlers ();
      fifo_reader_id_t prev = get_prev_owner ();
      if (!prev)
	{
	  debug_printf ("No previous owner to copy handles from");
	  return 0;
	}
      HANDLE prev_proc;
      if (prev.winpid == me.winpid)
	prev_proc =  GetCurrentProcess ();
      else
	prev_proc = OpenProcess (PROCESS_DUP_HANDLE, false, prev.winpid);
      if (!prev_proc)
	{
	  debug_printf ("Can't open process of previous owner, %E");
	  __seterrno ();
	  return -1;
	}

      for (int i = 0; i < get_shared_nhandlers (); i++)
	{
	  /* Should never happen. */
	  if (shared_fc_handler[i].state < fc_connected)
	    continue;
	  if (add_client_handler (false) < 0)
	    api_fatal ("Can't add client handler, %E");
	  fifo_client_handler &fc = fc_handler[nhandlers - 1];
	  if (!DuplicateHandle (prev_proc, shared_fc_handler[i].h,
				GetCurrentProcess (), &fc.h, 0,
				!close_on_exec (), DUPLICATE_SAME_ACCESS))
	    {
	      debug_printf ("Can't duplicate handle of previous owner, %E");
	      --nhandlers;
	      __seterrno ();
	      return -1;
	    }
	  fc.state = shared_fc_handler[i].state;
	}
    }
  return 0;
}

int
fhandler_fifo::update_shared_handlers ()
{
  cleanup_handlers ();
  if (nhandlers > get_shared_shandlers ())
    {
      if (remap_shared_fc_handler (nhandlers * sizeof (fc_handler[0])) < 0)
	return -1;
    }
  set_shared_nhandlers (nhandlers);
  memcpy (shared_fc_handler, fc_handler, nhandlers * sizeof (fc_handler[0]));
  return 0;
}

static DWORD WINAPI
fifo_reader_thread (LPVOID param)
{
  fhandler_fifo *fh = (fhandler_fifo *) param;
  return fh->fifo_reader_thread_func ();
}

DWORD
fhandler_fifo::fifo_reader_thread_func ()
{
  HANDLE conn_evt;

  if (!(conn_evt = CreateEvent (NULL, false, false, NULL)))
    api_fatal ("Can't create connection event, %E");

  while (1)
    {
      fifo_reader_id_t cur_owner;

      owner_lock ();
      cur_owner = get_owner ();
      if (!cur_owner)
	{
	  set_owner (me);
	  if (update_my_handlers () < 0)
	    api_fatal ("Can't update my handlers, %E");
	  owner_unlock ();
	  continue;
	}
      else if (cur_owner != me)
	{
	  owner_unlock ();
	  WaitForSingleObject (cancel_evt, INFINITE);
	  goto canceled;
	}
      else
	{
	  /* I'm the owner */
	  fifo_client_lock ();
	  cleanup_handlers ();
	  if (add_client_handler () < 0)
	    api_fatal ("Can't add a client handler, %E");

	  /* Listen for a writer to connect to the new client handler. */
	  fifo_client_handler& fc = fc_handler[nhandlers - 1];
	  fifo_client_unlock ();
	  owner_unlock ();
	  NTSTATUS status;
	  IO_STATUS_BLOCK io;
	  bool cancel = false;
	  bool update = false;

	  status = NtFsControlFile (fc.h, conn_evt, NULL, NULL, &io,
				    FSCTL_PIPE_LISTEN, NULL, 0, NULL, 0);
	  if (status == STATUS_PENDING)
	    {
	      HANDLE w[2] = { conn_evt, cancel_evt };
	      switch (WaitForMultipleObjects (2, w, false, INFINITE))
		{
		case WAIT_OBJECT_0:
		  status = io.Status;
		  debug_printf ("NtFsControlFile STATUS_PENDING, then %y",
				status);
		  break;
		case WAIT_OBJECT_0 + 1:
		  status = STATUS_THREAD_IS_TERMINATING;
		  cancel = true;
		  update = true;
		  break;
		default:
		  api_fatal ("WFMO failed, %E");
		}
	    }
	  else
	    debug_printf ("NtFsControlFile status %y, no STATUS_PENDING",
			  status);
	  HANDLE ph = NULL;
	  NTSTATUS status1;

	  fifo_client_lock ();
	  switch (status)
	    {
	    case STATUS_SUCCESS:
	    case STATUS_PIPE_CONNECTED:
	      record_connection (fc);
	      break;
	    case STATUS_PIPE_CLOSING:
	      record_connection (fc, fc_closing);
	      break;
	    case STATUS_THREAD_IS_TERMINATING:
	      /* Try to connect a bogus client.  Otherwise fc is still
		 listening, and the next connection might not get recorded. */
	      status1 = open_pipe (ph);
	      WaitForSingleObject (conn_evt, INFINITE);
	      if (NT_SUCCESS (status1))
		/* Bogus cilent connected. */
		delete_client_handler (nhandlers - 1);
	      else
		/* Did a real client connect? */
		switch (io.Status)
		  {
		  case STATUS_SUCCESS:
		  case STATUS_PIPE_CONNECTED:
		    record_connection (fc);
		    break;
		  case STATUS_PIPE_CLOSING:
		    record_connection (fc, fc_closing);
		    break;
		  default:
		    debug_printf ("NtFsControlFile status %y after failing to connect bogus client or real client", io.Status);
		    fc.state = fc_unknown;
		    break;
		  }
	      break;
	    default:
	      break;
	    }
	  fifo_client_unlock ();
	  if (ph)
	    NtClose (ph);
	  if (update && update_shared_handlers () < 0)
	    api_fatal ("Can't update shared handlers, %E");
	  if (cancel)
	    goto canceled;
	}
    }
canceled:
  if (conn_evt)
    NtClose (conn_evt);
  /* automatically return the cygthread to the cygthread pool */
  _my_tls._ctinfo->auto_release ();
  return 0;
}

int
fhandler_fifo::create_shmem ()
{
  HANDLE sect;
  OBJECT_ATTRIBUTES attr;
  NTSTATUS status;
  LARGE_INTEGER size = { .QuadPart = sizeof (fifo_shmem_t) };
  SIZE_T viewsize = sizeof (fifo_shmem_t);
  PVOID addr = NULL;
  UNICODE_STRING uname;
  WCHAR shmem_name[MAX_PATH];

  __small_swprintf (shmem_name, L"fifo-shmem.%08x.%016X", get_dev (),
		    get_ino ());
  RtlInitUnicodeString (&uname, shmem_name);
  InitializeObjectAttributes (&attr, &uname, OBJ_INHERIT,
			      get_shared_parent_dir (), NULL);
  status = NtCreateSection (&sect, STANDARD_RIGHTS_REQUIRED | SECTION_QUERY
			    | SECTION_MAP_READ | SECTION_MAP_WRITE,
			    &attr, &size, PAGE_READWRITE, SEC_COMMIT, NULL);
  if (status == STATUS_OBJECT_NAME_COLLISION)
    status = NtOpenSection (&sect, STANDARD_RIGHTS_REQUIRED | SECTION_QUERY
			    | SECTION_MAP_READ | SECTION_MAP_WRITE, &attr);
  if (!NT_SUCCESS (status))
    {
      __seterrno_from_nt_status (status);
      return -1;
    }
  status = NtMapViewOfSection (sect, NtCurrentProcess (), &addr, 0, viewsize,
			       NULL, &viewsize, ViewShare, 0, PAGE_READWRITE);
  if (!NT_SUCCESS (status))
    {
      NtClose (sect);
      __seterrno_from_nt_status (status);
      return -1;
    }
  shmem_handle = sect;
  shmem = (fifo_shmem_t *) addr;
  return 0;
}

/* shmem_handle must be valid when this is called. */
int
fhandler_fifo::reopen_shmem ()
{
  NTSTATUS status;
  SIZE_T viewsize = sizeof (fifo_shmem_t);
  PVOID addr = NULL;

  status = NtMapViewOfSection (shmem_handle, NtCurrentProcess (), &addr,
			       0, viewsize, NULL, &viewsize, ViewShare,
			       0, PAGE_READWRITE);
  if (!NT_SUCCESS (status))
    {
      __seterrno_from_nt_status (status);
      return -1;
    }
  shmem = (fifo_shmem_t *) addr;
  return 0;
}

/* On first creation, map and commit one page of memory. */
int
fhandler_fifo::create_shared_fc_handler ()
{
  HANDLE sect;
  OBJECT_ATTRIBUTES attr;
  NTSTATUS status;
  LARGE_INTEGER size
    = { .QuadPart = (LONGLONG) (SH_FC_HANDLER_PAGES * wincap.page_size ()) };
  SIZE_T viewsize = get_shared_fc_handler_committed () ?: wincap.page_size ();
  PVOID addr = NULL;
  UNICODE_STRING uname;
  WCHAR shared_fc_name[MAX_PATH];

  __small_swprintf (shared_fc_name, L"fifo-shared-fc.%08x.%016X", get_dev (),
		    get_ino ());
  RtlInitUnicodeString (&uname, shared_fc_name);
  InitializeObjectAttributes (&attr, &uname, OBJ_INHERIT,
			      get_shared_parent_dir (), NULL);
  status = NtCreateSection (&sect, STANDARD_RIGHTS_REQUIRED | SECTION_QUERY
			    | SECTION_MAP_READ | SECTION_MAP_WRITE, &attr,
			    &size, PAGE_READWRITE, SEC_RESERVE, NULL);
  if (status == STATUS_OBJECT_NAME_COLLISION)
    status = NtOpenSection (&sect, STANDARD_RIGHTS_REQUIRED | SECTION_QUERY
			    | SECTION_MAP_READ | SECTION_MAP_WRITE, &attr);
  if (!NT_SUCCESS (status))
    {
      __seterrno_from_nt_status (status);
      return -1;
    }
  status = NtMapViewOfSection (sect, NtCurrentProcess (), &addr, 0, viewsize,
			       NULL, &viewsize, ViewShare, 0, PAGE_READWRITE);
  if (!NT_SUCCESS (status))
    {
      NtClose (sect);
      __seterrno_from_nt_status (status);
      return -1;
    }
  shared_fc_hdl = sect;
  shared_fc_handler = (fifo_client_handler *) addr;
  if (!get_shared_fc_handler_committed ())
    set_shared_fc_handler_committed (viewsize);
  set_shared_shandlers (viewsize / sizeof (fifo_client_handler));
  return 0;
}

/* shared_fc_hdl must be valid when this is called. */
int
fhandler_fifo::reopen_shared_fc_handler ()
{
  NTSTATUS status;
  SIZE_T viewsize = get_shared_fc_handler_committed ();
  PVOID addr = NULL;

  status = NtMapViewOfSection (shared_fc_hdl, NtCurrentProcess (),
			       &addr, 0, viewsize, NULL, &viewsize,
			       ViewShare, 0, PAGE_READWRITE);
  if (!NT_SUCCESS (status))
    {
      __seterrno_from_nt_status (status);
      return -1;
    }
  shared_fc_handler = (fifo_client_handler *) addr;
  return 0;
}

int
fhandler_fifo::remap_shared_fc_handler (size_t nbytes)
{
  NTSTATUS status;
  SIZE_T viewsize = roundup2 (nbytes, wincap.page_size ());
  PVOID addr = NULL;

  if (viewsize > SH_FC_HANDLER_PAGES * wincap.page_size ())
    {
      set_errno (ENOMEM);
      return -1;
    }

  NtUnmapViewOfSection (NtCurrentProcess (), shared_fc_handler);
  status = NtMapViewOfSection (shared_fc_hdl, NtCurrentProcess (),
			       &addr, 0, viewsize, NULL, &viewsize,
			       ViewShare, 0, PAGE_READWRITE);
  if (!NT_SUCCESS (status))
    {
      __seterrno_from_nt_status (status);
      return -1;
    }
  shared_fc_handler = (fifo_client_handler *) addr;
  set_shared_fc_handler_committed (viewsize);
  set_shared_shandlers (viewsize / sizeof (fc_handler[0]));
  return 0;
}

int
fhandler_fifo::open (int flags, mode_t)
{
  int saved_errno = 0;

  if (flags & O_PATH)
    return open_fs (flags);

  /* Determine what we're doing with this fhandler: reading, writing, both */
  switch (flags & O_ACCMODE)
    {
    case O_RDONLY:
      reader = true;
      break;
    case O_WRONLY:
      writer = true;
      break;
    case O_RDWR:
      reader = true;
      duplexer = true;
      break;
    default:
      set_errno (EINVAL);
      goto err;
    }

  debug_only_printf ("reader %d, writer %d, duplexer %d", reader, writer, duplexer);
  set_flags (flags);
  if (reader && !duplexer)
    nohandle (true);

  /* Create control events for this named pipe */
  char char_sa_buf[1024];
  LPSECURITY_ATTRIBUTES sa_buf;
  sa_buf = sec_user_cloexec (flags & O_CLOEXEC, (PSECURITY_ATTRIBUTES) char_sa_buf,
		      cygheap->user.sid());

  char npbuf[MAX_PATH];
  __small_sprintf (npbuf, "r-event.%08x.%016X", get_dev (), get_ino ());
  if (!(read_ready = CreateEvent (sa_buf, true, false, npbuf)))
    {
      debug_printf ("CreateEvent for %s failed, %E", npbuf);
      __seterrno ();
      goto err;
    }
  npbuf[0] = 'w';
  if (!(write_ready = CreateEvent (sa_buf, true, false, npbuf)))
    {
      debug_printf ("CreateEvent for %s failed, %E", npbuf);
      __seterrno ();
      goto err_close_read_ready;
    }
  npbuf[0] = 'o';
  if (!(writer_opening = CreateEvent (sa_buf, true, false, npbuf)))
    {
      debug_printf ("CreateEvent for %s failed, %E", npbuf);
      __seterrno ();
      goto err_close_write_ready;
    }

  /* If we're reading, signal read_ready, create the shared memory,
     and start the fifo_reader_thread. */
  if (reader)
    {
      SetEvent (read_ready);
      if (create_shmem () < 0)
	goto err_close_writer_opening;
      if (create_shared_fc_handler () < 0)
	goto err_close_shmem;
      inc_nreaders ();
      if (!(cancel_evt = create_event (true)))
	goto err_dec_nreaders;
      if (!(thr_sync_evt = create_event (true)))
	goto err_close_cancel_evt;
      me.winpid = GetCurrentProcessId ();
      me.fh = this;
      new cygthread (fifo_reader_thread, this, "fifo_reader", thr_sync_evt);
      /* Wait until there's an owner. */
      owner_lock ();
      while (!get_owner ())
	{
	  owner_unlock ();
	  yield ();
	  owner_lock ();
	}
      owner_unlock ();

      /* If we're a duplexer, we need a handle for writing. */
      if (duplexer)
	{
	  HANDLE ph = NULL;
	  NTSTATUS status;

	  while (1)
	    {
	      status = open_pipe (ph);
	      if (NT_SUCCESS (status))
		{
		  set_handle (ph);
		  set_pipe_non_blocking (ph, flags & O_NONBLOCK);
		  break;
		}
	      else if (status == STATUS_OBJECT_NAME_NOT_FOUND)
		{
		  /* The pipe hasn't been created yet. */
		  yield ();
		  continue;
		}
	      else
		{
		  __seterrno_from_nt_status (status);
		  goto err_close_reader;
		}
	    }
	}
      /* Not a duplexer; wait for a writer to connect. */
      else if (!wait (write_ready))
	goto err_close_reader;
      goto success;
    }

  /* If we're writing, wait for read_ready, connect to the pipe, and
     signal write_ready.  */
  if (writer)
    {
      NTSTATUS status;

      SetEvent (writer_opening);
      if (!wait (read_ready))
	{
	  ResetEvent (writer_opening);
	  goto err_close_writer_opening;
	}
      while (1)
	{
	  status = open_pipe (get_handle ());
	  if (NT_SUCCESS (status))
	    goto writer_success;
	  else if (status == STATUS_OBJECT_NAME_NOT_FOUND)
	    {
	      /* The pipe hasn't been created yet. */
	      yield ();
	      continue;
	    }
	  else if (STATUS_PIPE_NO_INSTANCE_AVAILABLE (status))
	    break;
	  else
	    {
	      debug_printf ("create of writer failed");
	      __seterrno_from_nt_status (status);
	      ResetEvent (writer_opening);
	      goto err_close_writer_opening;
	    }
	}

      /* We should get here only if the system is heavily loaded
	 and/or many writers are trying to connect simultaneously */
      while (1)
	{
	  SetEvent (writer_opening);
	  if (!wait (read_ready))
	    {
	      ResetEvent (writer_opening);
	      goto err_close_writer_opening;
	    }
	  status = wait_open_pipe (get_handle ());
	  if (NT_SUCCESS (status))
	    goto writer_success;
	  else if (status == STATUS_IO_TIMEOUT)
	    continue;
	  else
	    {
	      debug_printf ("create of writer failed");
	      __seterrno_from_nt_status (status);
	      ResetEvent (writer_opening);
	      goto err_close_writer_opening;
	    }
	}
    }
writer_success:
  set_pipe_non_blocking (get_handle (), flags & O_NONBLOCK);
  SetEvent (write_ready);
success:
  return 1;
err_close_reader:
  saved_errno = get_errno ();
  close ();
  set_errno (saved_errno);
  return 0;
err_close_cancel_evt:
  NtClose (cancel_evt);
err_dec_nreaders:
  if (dec_nreaders () == 0)
    ResetEvent (read_ready);
/* err_close_shared_fc_handler: */
  NtUnmapViewOfSection (NtCurrentProcess (), shared_fc_handler);
  NtClose (shared_fc_hdl);
err_close_shmem:
  NtUnmapViewOfSection (NtCurrentProcess (), shmem);
  NtClose (shmem_handle);
err_close_writer_opening:
  NtClose (writer_opening);
err_close_write_ready:
  NtClose (write_ready);
err_close_read_ready:
  NtClose (read_ready);
err:
  if (get_handle ())
    NtClose (get_handle ());
  return 0;
}

off_t
fhandler_fifo::lseek (off_t offset, int whence)
{
  debug_printf ("(%D, %d)", offset, whence);
  set_errno (ESPIPE);
  return -1;
}

bool
fhandler_fifo::wait (HANDLE h)
{
#ifdef DEBUGGING
  const char *what;
  if (h == read_ready)
    what = "reader";
  else
    what = "writer";
#endif
  /* Set the wait to zero for non-blocking I/O-related events. */
  DWORD wait = ((h == read_ready || h == write_ready)
		&& get_flags () & O_NONBLOCK) ? 0 : INFINITE;

  debug_only_printf ("waiting for %s", what);
  /* Wait for the event.  Set errno, as appropriate if something goes wrong. */
  switch (cygwait (h, wait))
    {
    case WAIT_OBJECT_0:
      debug_only_printf ("successfully waited for %s", what);
      return true;
    case WAIT_SIGNALED:
      debug_only_printf ("interrupted by signal while waiting for %s", what);
      set_errno (EINTR);
      return false;
    case WAIT_CANCELED:
      debug_only_printf ("cancellable interruption while waiting for %s", what);
      pthread::static_cancel_self ();	/* never returns */
      break;
    case WAIT_TIMEOUT:
      if (h == write_ready)
	{
	  debug_only_printf ("wait timed out waiting for write but will still open reader since non-blocking mode");
	  return true;
	}
      else
	{
	  set_errno (ENXIO);
	  return false;
	}
      break;
    default:
      debug_only_printf ("unknown error while waiting for %s", what);
      __seterrno ();
      return false;
   }
}

ssize_t __reg3
fhandler_fifo::raw_write (const void *ptr, size_t len)
{
  ssize_t ret = -1;
  size_t nbytes = 0;
  ULONG chunk;
  NTSTATUS status = STATUS_SUCCESS;
  IO_STATUS_BLOCK io;
  HANDLE evt = NULL;

  if (!len)
    return 0;

  if (len <= max_atomic_write)
    chunk = len;
  else if (is_nonblocking ())
    chunk = len = max_atomic_write;
  else
    chunk = max_atomic_write;

  /* Create a wait event if the FIFO is in blocking mode. */
  if (!is_nonblocking () && !(evt = CreateEvent (NULL, false, false, NULL)))
    {
      __seterrno ();
      return -1;
    }

  /* Write in chunks, accumulating a total.  If there's an error, just
     return the accumulated total unless the first write fails, in
     which case return -1. */
  while (nbytes < len)
    {
      ULONG_PTR nbytes_now = 0;
      size_t left = len - nbytes;
      ULONG len1;
      DWORD waitret = WAIT_OBJECT_0;

      if (left > chunk)
	len1 = chunk;
      else
	len1 = (ULONG) left;
      nbytes_now = 0;
      status = NtWriteFile (get_handle (), evt, NULL, NULL, &io,
			    (PVOID) ptr, len1, NULL, NULL);
      if (evt && status == STATUS_PENDING)
	{
	  waitret = cygwait (evt);
	  if (waitret == WAIT_OBJECT_0)
	    status = io.Status;
	}
      if (waitret == WAIT_CANCELED)
	status = STATUS_THREAD_CANCELED;
      else if (waitret == WAIT_SIGNALED)
	status = STATUS_THREAD_SIGNALED;
      else if (isclosed ())  /* A signal handler might have closed the fd. */
	{
	  if (waitret == WAIT_OBJECT_0)
	    set_errno (EBADF);
	  else
	    __seterrno ();
	}
      else if (NT_SUCCESS (status))
	{
	  nbytes_now = io.Information;
	  /* NtWriteFile returns success with # of bytes written == 0
	     if writing on a non-blocking pipe fails because the pipe
	     buffer doesn't have sufficient space. */
	  if (nbytes_now == 0)
	    set_errno (EAGAIN);
	  ptr = ((char *) ptr) + chunk;
	  nbytes += nbytes_now;
	}
      else if (STATUS_PIPE_IS_CLOSED (status))
	{
	  set_errno (EPIPE);
	  raise (SIGPIPE);
	}
      else
	__seterrno_from_nt_status (status);
      if (nbytes_now == 0)
	len = 0;		/* Terminate loop. */
      if (nbytes > 0)
	ret = nbytes;
    }
  if (evt)
    NtClose (evt);
  if (status == STATUS_THREAD_SIGNALED && ret < 0)
    set_errno (EINTR);
  else if (status == STATUS_THREAD_CANCELED)
    pthread::static_cancel_self ();
  return ret;
}

/* A reader is at EOF if the pipe is empty and no writers are open.
   hit_eof is called by raw_read and select.cc:peek_fifo if it appears
   that we are at EOF after polling the fc_handlers.  We recheck this
   in case a writer opened while we were polling.  */
bool
fhandler_fifo::hit_eof ()
{
  bool ret = maybe_eof () && !IsEventSignalled (writer_opening);
  if (ret)
    {
      yield ();
      /* Wait for the reader thread to finish recording any connection. */
      fifo_client_lock ();
      fifo_client_unlock ();
      ret = maybe_eof ();
    }
  return ret;
}

void __reg3
fhandler_fifo::raw_read (void *in_ptr, size_t& len)
{
  if (!len)
    return;

  while (1)
    {
      /* Poll the connected clients for input. */
      int nconnected = 0;
      fifo_client_lock ();
      for (int i = 0; i < nhandlers; i++)
	if (fc_handler[i].state >= fc_connected)
	  {
	    NTSTATUS status;
	    IO_STATUS_BLOCK io;
	    size_t nbytes = 0;

	    nconnected++;
	    status = NtReadFile (fc_handler[i].h, NULL, NULL, NULL,
				 &io, in_ptr, len, NULL, NULL);
	    switch (status)
	      {
	      case STATUS_SUCCESS:
	      case STATUS_BUFFER_OVERFLOW:
		/* io.Information is supposedly valid. */
		nbytes = io.Information;
		if (nbytes > 0)
		  {
		    len = nbytes;
		    fifo_client_unlock ();
		    return;
		  }
		break;
	      case STATUS_PIPE_EMPTY:
		break;
	      case STATUS_PIPE_BROKEN:
		fc_handler[i].state = fc_disconnected;
		nconnected--;
		break;
	      default:
		debug_printf ("NtReadFile status %y", status);
		fc_handler[i].state = fc_error;
		nconnected--;
		break;
	      }
	  }
      maybe_eof (!nconnected && !IsEventSignalled (writer_opening));
      fifo_client_unlock ();
      if (maybe_eof () && hit_eof ())
	{
	  len = 0;
	  return;
	}
      if (is_nonblocking ())
	{
	  set_errno (EAGAIN);
	  goto errout;
	}
      else
	{
	  /* Allow interruption. */
	  DWORD waitret = cygwait (NULL, cw_nowait, cw_cancel | cw_sig_eintr);
	  if (waitret == WAIT_CANCELED)
	    pthread::static_cancel_self ();
	  else if (waitret == WAIT_SIGNALED)
	    {
	      if (_my_tls.call_signal_handler ())
		continue;
	      else
		{
		  set_errno (EINTR);
		  goto errout;
		}
	    }
	}
      /* We might have been closed by a signal handler or another thread. */
      if (isclosed ())
	{
	  set_errno (EBADF);
	  goto errout;
	}
      /* Don't hog the CPU. */
      Sleep (1);
    }
errout:
  len = (size_t) -1;
}

int __reg2
fhandler_fifo::fstatvfs (struct statvfs *sfs)
{
  if (get_flags () & O_PATH)
    /* We already have a handle. */
    {
      HANDLE h = get_handle ();
      if (h)
	return fstatvfs_by_handle (h, sfs);
    }

  fhandler_disk_file fh (pc);
  fh.get_device () = FH_FS;
  return fh.fstatvfs (sfs);
}

void
fhandler_fifo::close_all_handlers ()
{
  for (int i = 0; i < nhandlers; i++)
    fc_handler[i].close ();
  nhandlers = 0;
}

int
fifo_client_handler::pipe_state ()
{
  IO_STATUS_BLOCK io;
  FILE_PIPE_LOCAL_INFORMATION fpli;
  NTSTATUS status;

  status = NtQueryInformationFile (h, &io, &fpli,
				   sizeof (fpli), FilePipeLocalInformation);
  if (!NT_SUCCESS (status))
    {
      debug_printf ("NtQueryInformationFile status %y", status);
      __seterrno_from_nt_status (status);
      return -1;
    }
  else if (fpli.ReadDataAvailable > 0)
    return FILE_PIPE_INPUT_AVAILABLE_STATE;
  else
    return fpli.NamedPipeState;
}

void
fhandler_fifo::cancel_reader_thread ()
{
  if (cancel_evt)
    SetEvent (cancel_evt);
  if (thr_sync_evt)
    WaitForSingleObject (thr_sync_evt, INFINITE);
}

int
fhandler_fifo::close ()
{
  if (reader)
    {
      if (dec_nreaders () == 0)
	ResetEvent (read_ready);
      cancel_reader_thread ();
      owner_lock ();
      if (get_owner () == me)
	set_owner (null_fr_id);
      owner_unlock ();
      if (cancel_evt)
	NtClose (cancel_evt);
      if (thr_sync_evt)
	NtClose (thr_sync_evt);
      if (shmem)
	NtUnmapViewOfSection (NtCurrentProcess (), shmem);
      if (shmem_handle)
	NtClose (shmem_handle);
      if (shared_fc_handler)
	NtUnmapViewOfSection (NtCurrentProcess (), shared_fc_handler);
      if (shared_fc_hdl)
	NtClose (shared_fc_hdl);
    }
  if (read_ready)
    NtClose (read_ready);
  if (write_ready)
    NtClose (write_ready);
  if (writer_opening)
    NtClose (writer_opening);
  close_all_handlers ();
  if (fc_handler)
    free (fc_handler);
  return fhandler_base::close ();
}

/* If we have a write handle (i.e., we're a duplexer or a writer),
   keep the nonblocking state of the windows pipe in sync with our
   nonblocking state. */
int
fhandler_fifo::fcntl (int cmd, intptr_t arg)
{
  if (cmd != F_SETFL || nohandle () || (get_flags () & O_PATH))
    return fhandler_base::fcntl (cmd, arg);

  const bool was_nonblocking = is_nonblocking ();
  int res = fhandler_base::fcntl (cmd, arg);
  const bool now_nonblocking = is_nonblocking ();
  if (now_nonblocking != was_nonblocking)
    set_pipe_non_blocking (get_handle (), now_nonblocking);
  return res;
}

int
fhandler_fifo::dup (fhandler_base *child, int flags)
{
  fhandler_fifo *fhf = NULL;

  if (get_flags () & O_PATH)
    return fhandler_base::dup (child, flags);

  if (fhandler_base::dup (child, flags))
    goto err;

  fhf = (fhandler_fifo *) child;
  if (!DuplicateHandle (GetCurrentProcess (), read_ready,
			GetCurrentProcess (), &fhf->read_ready,
			0, !(flags & O_CLOEXEC), DUPLICATE_SAME_ACCESS))
    {
      __seterrno ();
      goto err;
    }
  if (!DuplicateHandle (GetCurrentProcess (), write_ready,
			GetCurrentProcess (), &fhf->write_ready,
			0, !(flags & O_CLOEXEC), DUPLICATE_SAME_ACCESS))
    {
      __seterrno ();
      goto err_close_read_ready;
    }
  if (!DuplicateHandle (GetCurrentProcess (), writer_opening,
			GetCurrentProcess (), &fhf->writer_opening,
			0, !(flags & O_CLOEXEC), DUPLICATE_SAME_ACCESS))
    {
      __seterrno ();
      goto err_close_write_ready;
    }
  if (reader)
    {
      /* Make sure the child starts unlocked. */
      fhf->fifo_client_unlock ();

      /* Clear fc_handler list; the child never starts as owner. */
      fhf->nhandlers = fhf->shandlers = 0;
      fhf->fc_handler = NULL;

      if (!DuplicateHandle (GetCurrentProcess (), shmem_handle,
			    GetCurrentProcess (), &fhf->shmem_handle,
			    0, !(flags & O_CLOEXEC), DUPLICATE_SAME_ACCESS))
	{
	  __seterrno ();
	  goto err_close_writer_opening;
	}
      if (fhf->reopen_shmem () < 0)
	goto err_close_shmem_handle;
      if (!DuplicateHandle (GetCurrentProcess (), shared_fc_hdl,
			    GetCurrentProcess (), &fhf->shared_fc_hdl,
			    0, !(flags & O_CLOEXEC), DUPLICATE_SAME_ACCESS))
	{
	  __seterrno ();
	  goto err_close_shmem;
	}
      if (fhf->reopen_shared_fc_handler () < 0)
	goto err_close_shared_fc_hdl;
      if (!(fhf->cancel_evt = create_event (true)))
	goto err_close_shared_fc_handler;
      if (!(fhf->thr_sync_evt = create_event (true)))
	goto err_close_cancel_evt;
      inc_nreaders ();
      fhf->me.fh = fhf;
      new cygthread (fifo_reader_thread, fhf, "fifo_reader", fhf->thr_sync_evt);
    }
  return 0;
err_close_cancel_evt:
  NtClose (fhf->cancel_evt);
err_close_shared_fc_handler:
  NtUnmapViewOfSection (GetCurrentProcess (), fhf->shared_fc_handler);
err_close_shared_fc_hdl:
  NtClose (fhf->shared_fc_hdl);
err_close_shmem:
  NtUnmapViewOfSection (GetCurrentProcess (), fhf->shmem);
err_close_shmem_handle:
  NtClose (fhf->shmem_handle);
err_close_writer_opening:
  NtClose (fhf->writer_opening);
err_close_write_ready:
  NtClose (fhf->write_ready);
err_close_read_ready:
  NtClose (fhf->read_ready);
err:
  return -1;
}

void
fhandler_fifo::fixup_after_fork (HANDLE parent)
{
  fhandler_base::fixup_after_fork (parent);
  fork_fixup (parent, read_ready, "read_ready");
  fork_fixup (parent, write_ready, "write_ready");
  fork_fixup (parent, writer_opening, "writer_opening");
  if (reader)
    {
      /* Make sure the child starts unlocked. */
      fifo_client_unlock ();

      fork_fixup (parent, shmem_handle, "shmem_handle");
      if (reopen_shmem () < 0)
	api_fatal ("Can't reopen shared memory during fork, %E");
      fork_fixup (parent, shared_fc_hdl, "shared_fc_hdl");
      if (reopen_shared_fc_handler () < 0)
	api_fatal ("Can't reopen shared fc_handler memory during fork, %E");
      if (close_on_exec ())
	/* Prevent a later attempt to close the non-inherited
	   pipe-instance handles copied from the parent. */
	nhandlers = 0;
      else
	{
	  /* Close inherited handles needed only by exec. */
	  if (cancel_evt)
	    NtClose (cancel_evt);
	  if (thr_sync_evt)
	    NtClose (thr_sync_evt);
	}
      if (!(cancel_evt = create_event (true)))
	api_fatal ("Can't create reader thread cancel event during fork, %E");
      if (!(thr_sync_evt = create_event (true)))
	api_fatal ("Can't create reader thread sync event during fork, %E");
      inc_nreaders ();
      me.winpid = GetCurrentProcessId ();
      new cygthread (fifo_reader_thread, this, "fifo_reader", thr_sync_evt);
    }
}

void
fhandler_fifo::fixup_after_exec ()
{
  fhandler_base::fixup_after_exec ();
  if (reader && !close_on_exec ())
    {
      /* Make sure the child starts unlocked. */
      fifo_client_unlock ();

      if (reopen_shmem () < 0)
	api_fatal ("Can't reopen shared memory during exec, %E");
      if (reopen_shared_fc_handler () < 0)
	api_fatal ("Can't reopen shared fc_handler memory during exec, %E");
      fc_handler = NULL;
      nhandlers = shandlers = 0;

      /* Cancel parent's reader thread */
      if (cancel_evt)
	SetEvent (cancel_evt);
      if (thr_sync_evt)
	WaitForSingleObject (thr_sync_evt, INFINITE);

      /* Take ownership if parent is owner. */
      fifo_reader_id_t parent_fr = me;
      me.winpid = GetCurrentProcessId ();
      owner_lock ();
      if (get_owner () == parent_fr)
	{
	  set_owner (me);
	  if (update_my_handlers (true) < 0)
	    api_fatal ("Can't update my handlers, %E");
	}
      owner_unlock ();
      /* Close inherited cancel_evt and thr_sync_evt. */
      if (cancel_evt)
	NtClose (cancel_evt);
      if (thr_sync_evt)
	NtClose (thr_sync_evt);
      if (!(cancel_evt = create_event (true)))
	api_fatal ("Can't create reader thread cancel event during exec, %E");
      if (!(thr_sync_evt = create_event (true)))
	api_fatal ("Can't create reader thread sync event during exec, %E");
      /* At this moment we're a new reader.  The count will be
	 decremented when the parent closes. */
      inc_nreaders ();
      new cygthread (fifo_reader_thread, this, "fifo_reader", thr_sync_evt);
    }
}

void
fhandler_fifo::set_close_on_exec (bool val)
{
  fhandler_base::set_close_on_exec (val);
  set_no_inheritance (read_ready, val);
  set_no_inheritance (write_ready, val);
  set_no_inheritance (writer_opening, val);
  if (reader)
    {
      set_no_inheritance (cancel_evt, val);
      set_no_inheritance (thr_sync_evt, val);
      fifo_client_lock ();
      for (int i = 0; i < nhandlers; i++)
	set_no_inheritance (fc_handler[i].h, val);
      fifo_client_unlock ();
    }
}
