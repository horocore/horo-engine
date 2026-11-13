/** @file RccppMacFileWatcher.cpp
 * @brief Private RCC++ kqueue adapter for in-place writes and atomic file replacement.
 */
#include "SimpleFileWatcher/FileWatcherOSX.h"

#include <array>
#include <cerrno>
#include <fcntl.h>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <sys/event.h>
#include <sys/stat.h>
#include <unistd.h>

namespace FW {
    /** @brief Owns every vnode descriptor for one non-recursive development directory. */
    struct WatchStruct {
        struct File {
            int descriptor;
            ino_t inode;
            timespec modified;
            off_t size;
        };

        WatchID id;
        String directory;
        FileWatchListener *listener;
        int queue;
        int descriptor{-1};
        std::map<std::string, File> files;

        WatchStruct(WatchID watchId, const String &path, FileWatchListener *owner, int eventQueue)
            : id(watchId), directory(path), listener(owner), queue(eventQueue) {}

        ~WatchStruct() {
            for (const auto &[name, file] : files)
                close(file.descriptor);
            if (descriptor >= 0)
                close(descriptor);
        }

        /** @brief Registers a vnode; failures remain explicit rather than silently disabling reload. */
        int Open(const std::string &path) const {
            const int file = open(path.c_str(), O_EVTONLY | O_CLOEXEC);
            if (file < 0)
                throw std::runtime_error("Unable to open RCC++ watched path: " + path);
            struct kevent event;
            EV_SET(&event, file, EVFILT_VNODE, EV_ADD | EV_CLEAR,
                   NOTE_WRITE | NOTE_EXTEND | NOTE_ATTRIB | NOTE_DELETE | NOTE_RENAME | NOTE_REVOKE, 0, nullptr);
            if (kevent(queue, &event, 1, nullptr, 0, nullptr) < 0) {
                close(file);
                throw std::runtime_error("Unable to register RCC++ watched path: " + path);
            }
            return file;
        }

        /** @brief Reconciles directory membership only after a directory event, including replaced inodes. */
        void Rescan(bool notify) {
            std::map<std::string, struct stat> present;
            std::error_code error;
            std::filesystem::directory_iterator entry{directory.c_str(), error};
            const std::filesystem::directory_iterator end;
            while (!error && entry != end) {
                struct stat status{};
                const auto path = entry->path().string();
                if (stat(path.c_str(), &status) == 0 && S_ISREG(status.st_mode))
                    present.emplace(path, status);
                entry.increment(error);
            }
            if (error)
                return;  // An interrupted save/removal will also deliver vnode events.
            for (auto current = files.begin(); current != files.end();) {
                const auto found = present.find(current->first);
                if (found == present.end() || found->second.st_ino != current->second.inode) {
                    if (notify)
                        listener->handleFileAction(id, directory, current->first, Actions::Delete);
                    close(current->second.descriptor);
                    current = files.erase(current);
                } else {
                    ++current;
                }
            }
            for (const auto &[path, status] : present) {
                if (files.contains(path))
                    continue;
                const int file = Open(path);
                files.emplace(path, File{file, status.st_ino, status.st_mtimespec, status.st_size});
                if (notify)
                    listener->handleFileAction(id, directory, path, Actions::Add);
            }
        }
    };

    /** @copydoc FileWatcherOSX::FileWatcherOSX */
    FileWatcherOSX::FileWatcherOSX() : mDescriptor(kqueue()), mTimeOut{}, mLastWatchID(0) {
        if (mDescriptor < 0)
            throw std::runtime_error("Unable to create RCC++ kqueue");
        fcntl(mDescriptor, F_SETFD, FD_CLOEXEC);
    }

    /** @copydoc FileWatcherOSX::~FileWatcherOSX */
    FileWatcherOSX::~FileWatcherOSX() {
        for (const auto &[id, watch] : mWatches)
            delete watch;
        close(mDescriptor);
    }

    /** @copydoc FileWatcherOSX::addWatch */
    WatchID FileWatcherOSX::addWatch(const String &directory, FileWatchListener *listener, bool recursive) {
        if (recursive)
            throw std::runtime_error("RCC++ macOS watches must be non-recursive");
        auto watch = std::make_unique<WatchStruct>(++mLastWatchID, directory, listener, mDescriptor);
        watch->descriptor = watch->Open(directory.m_string);
        watch->Rescan(false);
        const auto id = watch->id;
        mWatches.emplace(id, watch.get());
        watch.release();
        return id;
    }

    /** @copydoc FileWatcherOSX::removeWatch */
    void FileWatcherOSX::removeWatch(const String &directory) {
        for (const auto &[id, watch] : mWatches) {
            if (watch->directory == directory) {
                removeWatch(id);
                return;
            }
        }
    }

    /** @copydoc FileWatcherOSX::removeWatch */
    void FileWatcherOSX::removeWatch(WatchID id) {
        const auto found = mWatches.find(id);
        if (found != mWatches.end()) {
            delete found->second;
            mWatches.erase(found);
        }
    }

    /** @copydoc FileWatcherOSX::update */
    void FileWatcherOSX::update() {
        // Bounded and nonblocking; remaining events are handled on the next update.
        std::array<struct kevent, 64> events{};
        const int count = kevent(mDescriptor, nullptr, 0, events.data(), static_cast<int>(events.size()), &mTimeOut);
        if (count < 0) {
            if (errno != EINTR)
                throw std::runtime_error("RCC++ kqueue update failed");
            return;
        }
        for (int index = 0; index < count; ++index) {
            const auto &event = events[index];
            for (const auto &[id, watch] : mWatches) {
                if (event.ident == static_cast<uintptr_t>(watch->descriptor)) {
                    if (event.fflags & (NOTE_WRITE | NOTE_EXTEND | NOTE_DELETE | NOTE_RENAME | NOTE_REVOKE))
                        watch->Rescan(true);
                    break;
                }
                bool found = false;
                for (auto &[path, file] : watch->files) {
                    if (event.ident == static_cast<uintptr_t>(file.descriptor)) {
                        struct stat status{};
                        // Compiler reads can emit NOTE_ATTRIB for access time. Only content
                        // revision changes or revoked/replaced sources invalidate drawing.
                        const bool unavailable = fstat(file.descriptor, &status) != 0;
                        const bool changed = unavailable || status.st_size != file.size ||
                                             status.st_mtimespec.tv_sec != file.modified.tv_sec ||
                                             status.st_mtimespec.tv_nsec != file.modified.tv_nsec ||
                                             (event.fflags & (NOTE_DELETE | NOTE_RENAME | NOTE_REVOKE));
                        if (!unavailable) {
                            file.modified = status.st_mtimespec;
                            file.size = status.st_size;
                        }
                        if (changed)
                            watch->listener->handleFileAction(id, watch->directory, path, Actions::Modified);
                        found = true;
                        break;
                    }
                }
                if (found)
                    break;
            }
        }
    }

    /** @copydoc FileWatcherOSX::handleAction */
    void FileWatcherOSX::handleAction(WatchStruct *watch, const String &filename, unsigned long action) {
        watch->listener->handleFileAction(watch->id, watch->directory, filename, static_cast<Action>(action));
    }
}  // namespace FW
