/* This file is automatically generated with "make proto". DO NOT EDIT */

void child_run(struct child_struct *child);
void sync_parent(char *fname);
void nb_setup(struct child_struct *child);
void nb_unlink(struct child_struct *child, char *fname);
void nb_rmdir(struct child_struct *child, char *fname);
void nb_createx(struct child_struct *child, char *fname, 
		unsigned create_options, unsigned create_disposition, int fnum);
void nb_writex(struct child_struct *child, int handle, int offset, 
	       int size, int ret_size);
void nb_readx(struct child_struct *child, int handle, int offset, 
	      int size, int ret_size);
void nb_close(struct child_struct *child, int handle);
void nb_rename(struct child_struct *child, char *old, char *new);
void nb_flush(struct child_struct *child, int handle);
void nb_qpathinfo(struct child_struct *child, const char *fname);
void nb_qfileinfo(struct child_struct *child, int handle);
void nb_qfsinfo(struct child_struct *child, int level);
void nb_findfirst(struct child_struct *child, char *fname);
void nb_cleanup(struct child_struct *child);
void nb_deltree(struct child_struct *child, char *dname);
void do_unlink(char *fname);
void expand_file(int fd, int size);
void do_open(char *fname, int handle, int size);
void do_write(int handle, int size, int offset);
void do_read(int handle, int size, int offset);
void do_close(int handle);
void do_mkdir(char *fname);
void do_rmdir(char *fname);
void do_rename(char *old, char *new);
void do_stat(char *fname, int size);
void do_create(char *fname, int size);
void nb_setup(struct child_struct *child);
void nb_unlink(struct child_struct *child, char *fname);
void nb_rmdir(struct child_struct *child, char *fname);
void nb_createx(struct child_struct *child, char *fname, 
		unsigned create_options, unsigned create_disposition, int fnum);
void nb_writex(struct child_struct *child, int handle, int offset, 
	       int size, int ret_size);
void nb_readx(struct child_struct *child, int handle, int offset, 
	      int size, int ret_size);
void nb_close(struct child_struct *child, int handle);
void nb_rename(struct child_struct *child, char *old, char *new);
void nb_qpathinfo(struct child_struct *child, const char *fname);
void nb_qfileinfo(struct child_struct *child, int handle);
void nb_qfsinfo(struct child_struct *child, int level);
void nb_findfirst(struct child_struct *child, char *fname);
void nb_flush(struct child_struct *child, int handle);
void nb_cleanup(struct child_struct *child);
void nb_deltree(struct child_struct *child, char *dname);
int open_socket_in(int type, int port);
int open_socket_out(char *host, int port);
void set_socket_options(int fd, char *options);
int read_sock(int s, char *buf, int size);
int write_sock(int s, char *buf, int size);
void start_timer(void);
double end_timer(void);
void *shm_setup(int size);
void strupper(char *s);
void all_string_sub(char *s,const char *pattern,const char *insert);
BOOL next_token(char **ptr,char *buff,char *sep);
