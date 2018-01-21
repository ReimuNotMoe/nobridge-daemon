#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <cinttypes>
#include <cstring>

#include <unistd.h>
#include <dirent.h>
#include <fcntl.h>

#include <sys/inotify.h>

#include <utility>
#include <vector>
#include <unordered_map>


int fd_inotify = -1;
size_t len_basepath = 0;
const char *uid_android;

std::vector<std::pair<std::string, int>> MappingTable;
std::vector<long> GarbageTable;
std::unordered_map<std::string, long> LookupTable_path;
std::unordered_map<int, long> LookupTable_wd;

void TriggerMediaLibrary(const std::string &path_absolute) {

	auto pathc = path_absolute.c_str()+1+len_basepath;

	if (!(strstr(pathc, ".png") || strstr(pathc, ".JPG") || strstr(pathc, ".jpg") || strstr(pathc, ".PNG")))
		return;

//	std::string cmdline = "am broadcast -a android.intent.action.MEDIA_SCANNER_SCAN_FILE -d file:///sdcard'";
//	cmdline += pathc;
//	cmdline += "' --user ";
//	cmdline += uid_android;

	std::string cmdline = "file:///sdcard";
	cmdline += pathc;

//	fprintf(stderr, "TriggerMediaLibrary: system(\"%s\");\n", cmdline.c_str());

	auto pid = fork();

	if (pid) {
		fprintf(stderr, "TriggerMediaLibrary: forked pid %d\n", pid);
	} else {
		execl("/system/bin/am", "am", "broadcast", "-a", "android.intent.action.MEDIA_SCANNER_SCAN_FILE", "-d",
		      cmdline.c_str(), "--user", uid_android, NULL);
	}
}


void WatchList_RenameDirectory(int wd, std::string new_path_absolute) {
	auto it = LookupTable_wd.find(wd);

	long mt_idx = it->second;
	auto &path = MappingTable[mt_idx].first;

	LookupTable_path.erase(path);
	path = std::move(new_path_absolute);
	LookupTable_path.insert({path, mt_idx});

	fprintf(stderr, "WatchList::RenameDirectory: idx %ld, wd %d, newpath %s\n", mt_idx, wd, path.c_str());
}

void WatchList_AddDirectory(std::string path_absolute) {
	if (LookupTable_path.find(path_absolute) != LookupTable_path.end()) {
		fprintf(stderr, "WatchList::AddDirectory: !! path %s already exists\n", path_absolute.c_str());
		return;
	}

	int wd = inotify_add_watch(fd_inotify, path_absolute.c_str(), IN_CREATE | IN_MOVE_SELF | IN_MOVED_TO | IN_CLOSE_WRITE);

	if (LookupTable_wd.find(wd) == LookupTable_wd.end()) {
		MappingTable.emplace_back(std::pair<std::string, int>(path_absolute, wd));
		long mt_size = MappingTable.size() - 1;
		LookupTable_path.insert({path_absolute, mt_size});
		LookupTable_wd.insert({wd, mt_size});
		fprintf(stderr, "WatchList::AddDirectory: idx %ld, wd %d, path %s\n", mt_size, wd, path_absolute.c_str());
	} else {
		WatchList_RenameDirectory(wd, path_absolute);
	}


}

void WatchList_Remove(long idx, int wd, const std::string &path) {
	LookupTable_wd.erase(wd);
	LookupTable_path.erase(path);
	GarbageTable.push_back(idx);

	fprintf(stderr, "WatchList::Remove: idx %ld, wd %d, path %s\n", idx, wd, path.c_str());
	fprintf(stderr, "WatchList::Remove: GarbageTable size: %ld\n", GarbageTable.size());
}

void WatchList_RemoveByWd(int wd) {
	inotify_rm_watch(fd_inotify, wd);
	auto it = LookupTable_wd.find(wd);

	long mt_idx = it->second;
	auto path = MappingTable[mt_idx].first;

	WatchList_Remove(mt_idx, wd, path);
}

void WatchList_RemoveDirectoryByPrefix(const std::string &prefix) {

	for (size_t i=0; i<MappingTable.size(); i++) {
		auto &it = MappingTable[i];

		if (strstr(it.first.c_str(), prefix.c_str())) {
			WatchList_Remove(i, it.second, it.first);
		}
	}
}

void ScanExistingDir(const char *base_path, size_t buflen_path) {
	DIR *dir;
	struct dirent *entry;

	WatchList_AddDirectory(base_path);

	if (!(dir = opendir(base_path)))
		return;
	if (!(entry = readdir(dir)))
		return;

	char buf_path[buflen_path];

	do {
		if (entry->d_type == DT_DIR) {
			int len = snprintf(buf_path, buflen_path-1, "%s/%s", base_path, entry->d_name);
			buf_path[len] = 0;
			if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
				continue;
			ScanExistingDir(buf_path, buflen_path+1024);
		}
	} while (entry = readdir(dir));

	closedir(dir);
}


std::string GetPathByWd(int wd) {
	auto it = LookupTable_wd.find(wd);
	return MappingTable[it->second].first;
}

void WatchList_RemoveDirectoryByBaseWD(int wd) {
	auto path = GetPathByWd(wd);
	WatchList_RemoveDirectoryByPrefix(path);
	auto pathc = path.c_str();
	ScanExistingDir(std::string(pathc, strrchr(pathc, '/')-pathc).c_str(), path.size()+64);
}

void WaitDir(const char *p) {
	int fd;

retry:
	fd = open(p, O_DIRECTORY);

	if (fd == -1) {
		if (errno == ENOENT) {
			fprintf(stderr, "Main: Directory not found, will retry in 1 sec...\n");
			sleep(1);
			goto retry;
		} else {
			fprintf(stderr, "Main: Error: Failed to open `%s': %s\n", p, strerror(errno));
			exit(2);
		}
	} else {
		close(fd);
		return;
	}
}


int main(int argc, char **argv) {

	if (argc < 3) {
		fprintf(stderr, "Usage: nobridge-daemon <base_path> <android user id>\n");
		exit(1);
	}

	uid_android = argv[2];

	if (argv[1][0] != '/') {
		fprintf(stderr, "Main: Error: Absolute path required\n");
		exit(2);
	}

	WaitDir(argv[1]);

	fd_inotify = inotify_init();
	if (fd_inotify < 0) {
		fprintf(stderr, "Main: Error: Failed to init inotify: %s\n", strerror(errno));
		abort();
	}

	fprintf(stderr, "Main: Inotify inited, fd: %d\n", fd_inotify);

	len_basepath = strlen(argv[1]);

	ScanExistingDir((std::string("/") + std::string(argv[1])).c_str(), 1024);

	char buf_raw[4096] __attribute__ ((aligned(__alignof__(struct inotify_event))));
	struct inotify_event *buf_ev;

	ssize_t rc_read;


	while (1) {
		rc_read = read(fd_inotify, buf_raw, 4096);

		if (rc_read <= 0)
			break;


		for (char *ptr = buf_raw; ptr < buf_raw + rc_read; ptr += sizeof(struct inotify_event) + buf_ev->len) {

			buf_ev = (struct inotify_event *) ptr;

			fprintf(stderr, "Main: Loop: rc_read: %zd, mask: 0x%08x\n", rc_read, buf_ev->mask);

//			if (!(buf_ev->mask & IN_CREATE || buf_ev->mask & IN_IGNORED || buf_ev->mask & IN_CLOSE_WRITE
//			|| buf_ev->mask & IN_MOVE_SELF || buf_ev->mask & IN_MOVED_TO)) {
//				fprintf(stderr, "Main: Loop: Unwanted mask: 0x%08x\n", buf_ev->mask);
//				continue;
//			}

			if (buf_ev->mask & IN_MOVE_SELF) {
				WatchList_RemoveDirectoryByBaseWD(buf_ev->wd);
				continue;
			}

			std::string path_full = GetPathByWd(buf_ev->wd) + "/";
			path_full += std::string(buf_ev->name, buf_ev->len-1);

			printf("[%d] [0x%08x] %s : ", buf_ev->wd, buf_ev->mask, path_full.c_str());



			if (buf_ev->mask & IN_DELETE_SELF)
				printf("IN_DELETE_SELF, ");
			if (buf_ev->mask & IN_CREATE)
				printf("IN_CREATE, ");
			if (buf_ev->mask & IN_CLOSE_WRITE)
				printf("IN_CLOSE_WRITE, ");
			if (buf_ev->mask & IN_ISDIR)
				printf("IN_ISDIR, ");

			printf("\n");

			if (buf_ev->mask & IN_CLOSE_WRITE)
				TriggerMediaLibrary(path_full);

			if (buf_ev->mask & IN_ISDIR) {
				if (buf_ev->mask & IN_CREATE)
					WatchList_AddDirectory(path_full);
//				else if (buf_ev->mask & IN_MOVED_TO) {
//					WatchList_AddDirectory(path_full);
//				}
			}

			if (buf_ev->mask & IN_IGNORED) {
				// || buf_ev->mask & IN_MOVE_SELF
				WatchList_RemoveByWd(buf_ev->wd);
			}

		}

	}

}