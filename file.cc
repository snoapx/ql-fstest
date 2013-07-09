/************************************************************************
 *
 * Filesystem stress and verify
 *
 * Authors: Goswin von Brederlow <brederlo@informatik.uni-tuebingen.de>
 *          Bernd Schubert <bernd.schubert@fastmail.fm>
 *
 * Copyright (C) 2007 Q-leap Networks, Goswin von Brederlow
 *               2010 DataDirect Networks, Bernd Schubert
 *               2013 Fraunhofer ITWM, Bernd Schubert
 *
 *    This program is free software; you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation; either version 2 of the License, or
 *    (at your option) any later version.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with this program; if not, write to the Free Software
 *    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307
 *    USA
 *
 ************************************************************************/

#include "fstest.h"
#include "file.h"
#include "config.h"

const size_t BUF_SIZE = 1024*1024; // Must be power of 2

#define RANDOM_SIZE 4096

using namespace std;

File::File(Dir *dir)
{
	this->directory = dir;
	this->prev = NULL;
	this->next = NULL;
	this->num_checks = 0;
	this->sync_failed = false;
	this->has_error   = false;
	this->in_delete    = false;

	size_t size_min = get_global_cfg()->get_min_size_bits();
	size_t size_max = get_global_cfg()->get_max_size_bits();
	size_t random_size = random() % 4096;

	// Pick a random file size
	this->fsize = 1ULL << (size_min + random() % (size_max - size_min + 1));
	this->fsize += random_size; // do not let most of the the files have size of 2^n


	pthread_mutex_init(&this->mutex, NULL);
	
	// No need to lock the file here, as it is not globally known yet 
	// Initialization is just suffcient
	
	// according to man ctime_r we need at least 26 bytes
	this->time_buf = (char *) malloc(30); 
	if (this->time_buf == NULL) {
		cerr << "Out of memory while allocating a file" << endl;
		EXIT(1);
	}

	int fd;
	string path = dir->path();

	// Create file
retry:
	this->id.value = random();
	snprintf(fname, 9, "%x", id.value);

	fd = open((path + this->fname).c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
	if (fd == -1) {
		if (errno == EEXIST)
			goto retry; // Try again with new name
		std::cerr << "Creating file " << path << fname;
		perror(" : ");
		EXIT(1);
	}

	int rc = close(fd);
	if (rc)
		cerr << "Close " << path << fname << " failed: " << strerror(errno) << endl;

	// cout << "Path: " << path << fname <<" Size: " << this->fsize << endl;

}

/* Write a file here 
 * file needs to be locked already 
 */
void File::fwrite(void)
{
	int fd;
	int rc;
	bool immediate_check = get_global_cfg()->get_immediate_check();
	string path = directory->path();

	fd = open((path + this->fname).c_str(), O_RDWR);
	if (fd == -1) {
		std::cerr << "Writing file " << path << fname;
		perror(" : ");
		EXIT(1);
	}

	time_t rawtime;
	time(&rawtime);
	this->create_time = string(ctime_r(&rawtime, this->time_buf));

	string &tmp =  this->create_time;
	if (!tmp.empty() && tmp[tmp.length() - 1] == '\n')
		tmp.erase(tmp.length() - 1); // remove "\n"

	// Create buffer and fill with id
	char *buf = (char *)malloc(BUF_SIZE);
	if (!buf) {
		cerr << "malloc failed" << endl;
		EXIT(1);
	}
	size_t size = sizeof(this->id.checksum);

	memcpy(&buf[0], this->id.checksum, size);
	while(size < BUF_SIZE) {
		memcpy(&buf[size], &buf[0], size);
		size *= 2;
	}
	// write file

	loff_t file_offset = 0;
	bool file_end = false;
	while ( ( (uint64_t) file_offset < this->fsize) && !file_end) {
		size_t buf_offset = 0;
		while (buf_offset < BUF_SIZE && !file_end) {

			size_t remaining_buf_len = BUF_SIZE - buf_offset;
			size_t write_len = remaining_buf_len;

			if (file_offset + write_len > this->fsize) {
				write_len = this->fsize - file_offset;
				file_end = true;
			}

			ssize_t written_len = write(fd, &buf[buf_offset], write_len);
			if (written_len < 0) {
				if (errno == ENOSPC) {
					cout << path << fname 
						<< ": Out of disk space, "
						<< "probably a race with another thread" << endl;
					goto out;
				}
				cerr << "Write to " << path << fname << " failed";
				perror(" : ");
				EXIT(1);
			}

			buf_offset  += written_len;
			file_offset += written_len;

			if ((size_t) file_offset > this->fsize) {
				cerr << "Bug: Wrote more than we should write!: " <<
					path << fname << endl;
			}

		}

		// cout << "file_offset: " << file_offset << " goal-fsize: " << this->fsize << endl;
	}

out:
	rc = fdatasync(fd);
	if (rc) {
		cerr << "fdatasync() " << path << this->fname 
			<< " failed (rc = " << rc << "): " 
			<< strerror(errno) <<endl;
		this->sync_failed = true;
	}
	

	// Try to remove pages from memory to let the kernel re-read the file
	// from disk on later reads
	posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);

	if (immediate_check) {
		errno = 0; // reset errno
		lseek(fd, 0, SEEK_SET);
		this->check_fd(fd); // immediately check the file now, TODO: make this an option
	}

	rc = close(fd);
	if (rc) {
		cerr << "close() " << path << this->fname 
			<< " failed: (rc = " << rc << "): "
			<< strerror(errno) << endl;
		this->sync_failed = true;
	}
	free(buf);

}


/* file destructor - delete a file
 * the filesystem has to be locked before calling this
 */
File::~File(void)
{
#ifdef DEBUG
	cout << "~File(" << this->directory->path() + this->fname << ")" << endl;
#endif

	if (this->has_error) {
		cout << "Refusing to delete " 
			<< this->directory->path() + this->fname << endl;
		this->unlock();
		RETURNV;
	}

	// Remove from dir
	directory->remove_file(this);
	// delete file
	if (::unlink((directory->path() + fname).c_str()) != 0)
	{
		cerr << "Deleting file " << directory->path() << fname << " failed:" <<
			strerror(errno) << std::endl;
		if (errno != ENOENT)
			EXIT(1);

	}

	free(this->time_buf);

	this->unlock();
	pthread_mutex_destroy(&this->mutex);
}

void File::delete_all(void)
{
	File *p = this;
	while(p != NULL) {
		File *n = p->next;
		delete p;
		p = n;
	}
}

void File::link(File *file)
{
	next = file;
	if (next != NULL) {
		prev = next->prev;
		next->prev = this;
	}
	if (prev != NULL) prev->next = this;
}

void File::unlink()
{
	if (prev != NULL) prev->next = next;
	if (next != NULL) next->prev = prev;
	prev = next = NULL;
}

/* check the given file descriptor for corruption
 * no locking magic here, this function just does the checking of an opened file
 */
int File::check_fd(int fd)
{
	int ret = 0;

	// Do not keep the pages in memory, later checks then have to re-read it.
	// Disadvantage is that we do not create memory pressure then, which is
	// usually good to stress test filesystems
	posix_fadvise(fd, 0 ,0, POSIX_FADV_NOREUSE);

	//Create buffer and fill with id
	char *bufm = (char *)malloc(BUF_SIZE);
	char *buff = (char *)malloc(BUF_SIZE);
	if (!bufm or !buff) {
		cerr << "Malloc failed" << endl;
		EXIT(1);
	}

	size_t size = sizeof(this->id.checksum);
	
	memcpy(&bufm[0], this->id.checksum, size);
	while(size < BUF_SIZE) {
		memcpy(&bufm[size], &bufm[0], size);
		size *= 2;
	}
	
	// read and compare file
	uint64_t file_read_size = 0;
	for(size = 0; size < this->fsize; size += BUF_SIZE) {
		size_t buf_offset = 0;
		while (buf_offset < BUF_SIZE) {
			ssize_t read_len;
			read_len = read(fd, &buff[buf_offset], BUF_SIZE - buf_offset);
			if (read_len < 0) {
				cerr << "Read from " << directory->path() 
					<< fname << " failed: " 
					<< strerror(errno) << endl;
				ret = 1;
				goto out;
			}

			file_read_size += read_len;

			if (read_len == 0) {
				if (file_read_size < this->fsize)
					cerr << "File smaller than expected: " <<
						directory->path() << fname 	<<
						" expected: " << this->fsize	<<
						" got: " << file_read_size <<endl;
				ret = 0;
				goto out;
			}

			if (file_read_size > this->fsize) {
				if (file_read_size < this->fsize)
					cerr << "File larger than expected: " <<
						directory->path() << fname 	<<
						" expected: " << this->fsize	<<
						" got: " << file_read_size <<endl;
				ret = 0;
				goto out;
			}


			buf_offset += read_len;
#if 0
			cout << "Read: " << read_len 		<< " " <<
				"Buf-Offset: "    << buf_offset << " " <<
				"File-readsize: " << file_read_size << " " <<
				"File-Size: " << this->fsize << endl;
#endif

		}

		// If the filesystem was full, not the complete file was written
		// and so the file might not have a size being a multiple of
		// BUF_SIZE. So introduce a cmp size.
		size_t cmpsize;
		cmpsize = min(BUF_SIZE, this->fsize - size);
		if (memcmp(bufm, buff, cmpsize) != 0) {
			this->has_error = true;
			cerr << "File corruption in " 
				<< directory->path() << this->fname
				<< " (create time: " << this->create_time << ")"
			        << " around " << size << " [pattern = "
			        << std::hex << id.value << std::dec << "]" << endl;
			cerr << "After n-checks: " <<  this->num_checks << endl;
			for (unsigned ia = 0; ia < BUF_SIZE; ia++) {
				if (memcmp(bufm + ia, buff + ia, 1) != 0) {
					fprintf(stderr, "Expected: %x, got: %x (pos = %lu)\n",
					        (unsigned char) bufm[ia], (unsigned char) buff[ia],
					        (long unsigned) size + ia);
				}
			}
			// Do not RETURN an error and abort writes, if we know
			// this sync to disk of this file failed
			if (!this->sync_failed) {
				ret = 1;
				goto out;
			}
		}
	}

out:
	// Try to remove pages from memory to let the kernel re-read the file
	// on later reads
	posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);

	free(bufm);
	free(buff);
	RETURN(ret);
}

/* check the file for corruption
 * the file MUST be locked before calling this method
 */
int File::check(void)
{
#ifdef DEBUG
	cerr << " Checking file " << this->directory->path() << this->fname << endl;
#endif

	if (this->has_error)
		RETURN(0); // No need to further check this

	if (this->trylock() != EBUSY)
		cout << "Program error:  file is not locked " << this->fname << endl;
	
	int fd = open((directory->path() + fname).c_str(), O_RDONLY);
	if (fd == -1) {
		cerr << " Checking file " << this->directory->path() << this->fname;
		perror(" : ");
		EXIT(1);
	}

	int ret = this->check_fd(fd);

	close(fd);
	
	this->num_checks++;

	RETURN(ret);
}

File * File::get_next() const
{
	return this->next;
}

void File::lock(void)
{
	int rc = pthread_mutex_lock(&this->mutex);
	if (rc) {
		cerr << "Failed to lock " << this->fname << " : " << strerror(rc);
		perror(" : ");
		EXIT(1);
	}
}

void File::unlock(void)
{
	int rc = pthread_mutex_unlock(&this->mutex);
	if (rc) {
		cerr << "Failed to lock " << this->fname << " : " << strerror(rc);
		perror(" : ");
		EXIT(1);
	}
}

int File::trylock(void)
{
	int rc = pthread_mutex_trylock(&this->mutex);
	if (rc && rc != EBUSY) {
		cerr << "Failed to lock " << this->fname << " : " << strerror(rc);
		perror(" : ");
		EXIT(1);
	}
	RETURN(rc);
}

