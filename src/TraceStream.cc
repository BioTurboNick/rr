/* -*- Mode: C++; tab-width: 8; c-basic-offset: 2; indent-tabs-mode: nil; -*- */

#include "TraceStream.h"

#include <capnp/message.h>
#include <capnp/serialize-packed.h>
#include <inttypes.h>
#include <limits.h>
#include <sysexits.h>

#include <algorithm>
#include <fstream>
#include <sstream>
#include <string>

#include "AddressSpace.h"
#include "RecordSession.h"
#include "RecordTask.h"
#include "TaskishUid.h"
#include "kernel_supplement.h"
#include "log.h"
#include "rr_trace.capnp.h"
#include "util.h"

using namespace std;
using namespace capnp;

namespace rr {

//
// This represents the format and layout of recorded traces.  This
// version number doesn't track the rr version number, because changes
// to the trace format will be rare.
//
// NB: if you *do* change the trace format for whatever reason, you
// MUST increment this version number.  Otherwise users' old traces
// will become unreplayable and they won't know why.
//
#define TRACE_VERSION 85

struct SubstreamData {
  const char* name;
  size_t block_size;
  int threads;
};

static SubstreamData substreams[TraceStream::SUBSTREAM_COUNT] = {
  { "events", 1024 * 1024, 1 }, { "data_header", 1024 * 1024, 1 },
  { "data", 1024 * 1024, 0 },   { "mmaps", 64 * 1024, 1 },
  { "tasks", 64 * 1024, 1 },    { "generic", 64 * 1024, 1 },
};

static const SubstreamData& substream(TraceStream::Substream s) {
  if (!substreams[TraceStream::RAW_DATA].threads) {
    substreams[TraceStream::RAW_DATA].threads = min(8, get_num_cpus());
  }
  return substreams[s];
}

static TraceStream::Substream operator++(TraceStream::Substream& s) {
  s = (TraceStream::Substream)(s + 1);
  return s;
}

static bool dir_exists(const string& dir) {
  struct stat dummy;
  return !dir.empty() && stat(dir.c_str(), &dummy) == 0;
}

static string default_rr_trace_dir() {
  static string cached_dir;

  if (!cached_dir.empty()) {
    return cached_dir;
  }

  string dot_dir;
  const char* home = getenv("HOME");
  if (home) {
    dot_dir = string(home) + "/.rr";
  }
  string xdg_dir;
  const char* xdg_data_home = getenv("XDG_DATA_HOME");
  if (xdg_data_home) {
    xdg_dir = string(xdg_data_home) + "/rr";
  } else if (home) {
    xdg_dir = string(home) + "/.local/share/rr";
  }

  // If XDG dir does not exist but ~/.rr does, prefer ~/.rr for backwards
  // compatibility.
  if (dir_exists(xdg_dir)) {
    cached_dir = xdg_dir;
  } else if (dir_exists(dot_dir)) {
    cached_dir = dot_dir;
  } else if (!xdg_dir.empty()) {
    cached_dir = xdg_dir;
  } else {
    cached_dir = "/tmp/rr";
  }

  return cached_dir;
}

static string trace_save_dir() {
  const char* output_dir = getenv("_RR_TRACE_DIR");
  return output_dir ? output_dir : default_rr_trace_dir();
}

static string latest_trace_symlink() {
  return trace_save_dir() + "/latest-trace";
}

static void ensure_dir(const string& dir, mode_t mode) {
  string d = dir;
  while (!d.empty() && d[d.length() - 1] == '/') {
    d = d.substr(0, d.length() - 1);
  }

  struct stat st;
  if (0 > stat(d.c_str(), &st)) {
    if (errno != ENOENT) {
      FATAL() << "Error accessing trace directory `" << dir << "'";
    }

    size_t last_slash = d.find_last_of('/');
    if (last_slash == string::npos || last_slash == 0) {
      FATAL() << "Can't find trace directory `" << dir << "'";
    }
    ensure_dir(d.substr(0, last_slash), mode);

    // Allow for a race condition where someone else creates the directory
    if (0 > mkdir(d.c_str(), mode) && errno != EEXIST) {
      FATAL() << "Can't create trace directory `" << dir << "'";
    }
    if (0 > stat(d.c_str(), &st)) {
      FATAL() << "Can't stat trace directory `" << dir << "'";
    }
  }

  if (!(S_IFDIR & st.st_mode)) {
    FATAL() << "`" << dir << "' exists but isn't a directory.";
  }
  if (access(d.c_str(), W_OK)) {
    FATAL() << "Can't write to `" << dir << "'.";
  }
}

/**
 * Create the default ~/.rr directory if it doesn't already exist.
 */
static void ensure_default_rr_trace_dir() {
  ensure_dir(default_rr_trace_dir(), S_IRWXU);
}

class CompressedWriterOutputStream : public kj::OutputStream {
public:
  CompressedWriterOutputStream(CompressedWriter& writer) : writer(writer) {}
  virtual ~CompressedWriterOutputStream() {}

  virtual void write(const void* buffer, size_t size) {
    writer.write(buffer, size);
  }

private:
  CompressedWriter& writer;
};

struct IOException {};

class CompressedReaderInputStream : public kj::BufferedInputStream {
public:
  CompressedReaderInputStream(CompressedReader& reader) : reader(reader) {}
  virtual ~CompressedReaderInputStream() {}

  virtual size_t tryRead(void* buffer, size_t, size_t maxBytes) {
    if (!reader.read(buffer, maxBytes)) {
      throw IOException();
    }
    return maxBytes;
  }
  virtual void skip(size_t bytes) {
    if (!reader.skip(bytes)) {
      throw IOException();
    }
  }
  virtual kj::ArrayPtr<const byte> tryGetReadBuffer() {
    const uint8_t* p;
    size_t size;
    if (!reader.get_buffer(&p, &size)) {
      throw IOException();
    }
    return kj::ArrayPtr<const byte>(p, size);
  }

private:
  CompressedReader& reader;
};

TraceStream::TraceStream(const string& trace_dir, FrameTime initial_time)
    : trace_dir(real_path(trace_dir)), global_time(initial_time) {}

string TraceStream::file_data_clone_file_name(const TaskUid& tuid) {
  stringstream ss;
  ss << trace_dir << "/cloned_data_" << tuid.tid() << "_" << tuid.serial();
  return ss.str();
}

string TraceStream::path(Substream s) {
  return trace_dir + "/" + substream(s).name;
}

size_t TraceStream::mmaps_block_size() { return substreams[MMAPS].block_size; }

bool TraceWriter::good() const {
  for (auto& w : writers) {
    if (!w->good()) {
      return false;
    }
  }
  return true;
}

bool TraceReader::good() const {
  for (auto& r : readers) {
    if (!r->good()) {
      return false;
    }
  }
  return true;
}

static kj::ArrayPtr<const byte> str_to_data(const string& str) {
  return kj::ArrayPtr<const byte>(reinterpret_cast<const byte*>(str.data()), str.size());
}

static string data_to_str(const kj::ArrayPtr<const byte>& data) {
  return string(reinterpret_cast<const char*>(data.begin()), data.size());
}

struct BasicInfo {
  FrameTime global_time;
  pid_t tid_;
  EncodedEvent ev;
  Ticks ticks_;
  double monotonic_sec;
};

void TraceWriter::write_frame(const TraceFrame& frame) {
  auto& events = writer(EVENTS);

  BasicInfo basic_info;
  memset(&basic_info, 0, sizeof(BasicInfo));
  basic_info.global_time = frame.time();
  basic_info.tid_ = frame.tid();
  basic_info.ev = frame.event().encode();
  basic_info.ticks_ = frame.ticks();
  basic_info.monotonic_sec = frame.monotonic_time();
  events << basic_info;
  if (!events.good()) {
    FATAL() << "Tried to save " << sizeof(basic_info)
            << " bytes to the trace, but failed";
  }
  if (frame.event().has_exec_info() == HAS_EXEC_INFO) {
    events << (char)frame.regs().arch();
    // Avoid dynamic allocation and copy
    auto raw_regs = frame.regs().get_ptrace_for_self_arch();
    events.write(raw_regs.data, raw_regs.size);
    if (!events.good()) {
      FATAL() << "Tried to save registers to the trace, but failed";
    }

    int extra_reg_bytes = frame.extra_regs().data_size();
    char extra_reg_format = (char)frame.extra_regs().format();
    events << extra_reg_format << extra_reg_bytes;
    if (!events.good()) {
      FATAL() << "Tried to save "
              << sizeof(extra_reg_bytes) + sizeof(extra_reg_format)
              << " bytes to the trace, but failed";
    }
    if (extra_reg_bytes > 0) {
      events.write((const char*)frame.extra_regs().data_bytes(),
                   extra_reg_bytes);
      if (!events.good()) {
        FATAL() << "Tried to save " << extra_reg_bytes
                << " bytes to the trace, but failed";
      }
    }
  }

  tick_time();
}

TraceFrame TraceReader::read_frame() {
  // Read the common event info first, to see if we also have
  // exec info to read.
  auto& events = reader(EVENTS);
  BasicInfo basic_info;
  events >> basic_info;
  TraceFrame frame(basic_info.global_time, basic_info.tid_,
                   Event(basic_info.ev), basic_info.ticks_,
                   basic_info.monotonic_sec);
  if (frame.event().has_exec_info() == HAS_EXEC_INFO) {
    char a;
    events >> a;
    uint8_t buf[sizeof(X64Arch::user_regs_struct)];
    frame.recorded_regs.set_arch((SupportedArch)a);
    switch (frame.recorded_regs.arch()) {
      case x86:
        events.read(buf, sizeof(X86Arch::user_regs_struct));
        frame.recorded_regs.set_from_ptrace_for_arch(
            x86, buf, sizeof(X86Arch::user_regs_struct));
        break;
      case x86_64:
        events.read(buf, sizeof(X64Arch::user_regs_struct));
        frame.recorded_regs.set_from_ptrace_for_arch(
            x86_64, buf, sizeof(X64Arch::user_regs_struct));
        break;
      default:
        FATAL() << "Unknown arch";
    }

    int extra_reg_bytes;
    char extra_reg_format;
    events >> extra_reg_format >> extra_reg_bytes;
    if (extra_reg_bytes > 0) {
      vector<uint8_t> data;
      data.resize(extra_reg_bytes);
      events.read((char*)data.data(), extra_reg_bytes);
      bool ok = frame.recorded_extra_regs.set_to_raw_data(
          frame.event().arch(), (ExtraRegisters::Format)extra_reg_format, data,
          xsave_layout_from_trace(cpuid_records()));
      if (!ok) {
        FATAL() << "Invalid XSAVE data in trace";
      }
    } else {
      assert(extra_reg_format == ExtraRegisters::NONE);
      frame.recorded_extra_regs = ExtraRegisters(frame.event().arch());
    }
  }

  tick_time();
  assert(time() == frame.time());
  return frame;
}

void TraceWriter::write_task_event(const TraceTaskEvent& event) {
  MallocMessageBuilder task_msg;
  trace::TaskEvent::Builder task = task_msg.initRoot<trace::TaskEvent>();
  task.setFrameTime(global_time);
  task.setTid(event.tid());

  switch (event.type()) {
    case TraceTaskEvent::CLONE: {
      auto clone = task.initClone();
      clone.setParentTid(event.parent_tid());
      clone.setOwnNsTid(event.own_ns_tid());
      clone.setFlags(event.clone_flags());
      break;
    }
    case TraceTaskEvent::EXEC: {
      auto exec = task.initExec();
      exec.setFileName(str_to_data(event.file_name()));
      const auto& event_cmd_line = event.cmd_line();
      auto cmd_line = exec.initCmdLine(event_cmd_line.size());
      for (size_t i = 0; i < event_cmd_line.size(); ++i) {
        cmd_line.set(i, str_to_data(event_cmd_line[i]));
      }
      break;
    }
    case TraceTaskEvent::EXIT:
      task.initExit().setExitStatus(event.exit_status().get());
      break;
    case TraceTaskEvent::NONE:
      assert(0 && "Writing NONE TraceTaskEvent");
      break;
  }

  try {
    auto& tasks = writer(TASKS);
    CompressedWriterOutputStream stream(tasks);
    writePackedMessage(stream, task_msg);
  } catch (...) {
    FATAL() << "Unable to write tasks";
  }
}

static pid_t i32_to_tid(int tid) {
  if (tid <= 0) {
    FATAL() << "Invalid tid";
  }
  return tid;
}

TraceTaskEvent TraceReader::read_task_event() {
  TraceTaskEvent r;
  auto& tasks = reader(TASKS);
  if (tasks.at_end()) {
    return r;
  }

  CompressedReaderInputStream stream(tasks);
  PackedMessageReader task_msg(stream);
  trace::TaskEvent::Reader task = task_msg.getRoot<trace::TaskEvent>();
  r.tid_ = i32_to_tid(task.getTid());
  switch (task.which()) {
    case trace::TaskEvent::Which::CLONE: {
      r.type_ = TraceTaskEvent::CLONE;
      auto clone = task.getClone();
      r.parent_tid_ = i32_to_tid(clone.getParentTid());
      r.own_ns_tid_ = i32_to_tid(clone.getOwnNsTid());
      r.clone_flags_ = clone.getFlags();
      break;
    }
    case trace::TaskEvent::Which::EXEC: {
      r.type_ = TraceTaskEvent::EXEC;
      auto exec = task.getExec();
      r.file_name_ = data_to_str(exec.getFileName());
      auto cmd_line = exec.getCmdLine();
      r.cmd_line_.resize(cmd_line.size());
      for (size_t i = 0; i < cmd_line.size(); ++i) {
        r.cmd_line_[i] = data_to_str(cmd_line[i]);
      }
      break;
    }
    case trace::TaskEvent::Which::EXIT:
      r.type_ = TraceTaskEvent::EXIT;
      r.exit_status_ = WaitStatus(task.getExit().getExitStatus());
      break;
    default:
      assert(0 && "Unknown TraceEvent type");
      break;
  }
  return r;
}

static string base_file_name(const string& file_name) {
  size_t last_slash = file_name.rfind('/');
  return (last_slash != file_name.npos) ? file_name.substr(last_slash + 1)
                                        : file_name;
}

string TraceWriter::try_hardlink_file(const string& file_name) {
  char count_str[20];
  sprintf(count_str, "%d", mmap_count);

  string path =
      string("mmap_hardlink_") + count_str + "_" + base_file_name(file_name);
  int ret = link(file_name.c_str(), (dir() + "/" + path).c_str());
  if (ret < 0) {
    // maybe tried to link across filesystems?
    return file_name;
  }
  return path;
}

bool TraceWriter::try_clone_file(RecordTask* t, const string& file_name,
                                 string* new_name) {
  if (!t->session().use_file_cloning()) {
    return false;
  }

  char count_str[20];
  sprintf(count_str, "%d", mmap_count);

  string path =
      string("mmap_clone_") + count_str + "_" + base_file_name(file_name);

  ScopedFd src(file_name.c_str(), O_RDONLY);
  if (!src.is_open()) {
    return false;
  }
  string dest_path = dir() + "/" + path;
  ScopedFd dest(dest_path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0700);
  if (!dest.is_open()) {
    return false;
  }

  int ret = ioctl(dest, BTRFS_IOC_CLONE, src.get());
  if (ret < 0) {
    // maybe not on the same filesystem, or filesystem doesn't support clone?
    unlink(dest_path.c_str());
    return false;
  }

  *new_name = path;
  return true;
}

TraceWriter::RecordInTrace TraceWriter::write_mapped_region(
    RecordTask* t, const KernelMapping& km, const struct stat& stat,
    MappingOrigin origin) {
  MallocMessageBuilder map_msg;
  trace::MMap::Builder map = map_msg.initRoot<trace::MMap>();
  map.setFrameTime(global_time);
  map.setStart(km.start().as_int());
  map.setEnd(km.end().as_int());
  map.setFsname(str_to_data(km.fsname()));
  map.setDevice(km.device());
  map.setInode(km.inode());
  map.setProt(km.prot());
  map.setFlags(km.flags());
  map.setFileOffsetBytes(km.file_offset_bytes());
  map.setStatMode(stat.st_mode);
  map.setStatUid(stat.st_uid);
  map.setStatGid(stat.st_gid);
  map.setStatSize(stat.st_size);
  map.setStatMTime(stat.st_mtime);
  auto src = map.getSource();
  string backing_file_name;

  if (origin == REMAP_MAPPING || origin == PATCH_MAPPING) {
    src.setZero();
  } else if (km.fsname().find("/SYSV") == 0) {
    src.setTrace();
  } else if (origin == SYSCALL_MAPPING &&
             (km.inode() == 0 || km.fsname() == "/dev/zero (deleted)")) {
    src.setZero();
  } else if (origin == RR_BUFFER_MAPPING) {
    src.setZero();
  } else if ((km.flags() & MAP_PRIVATE) &&
             try_clone_file(t, km.fsname(), &backing_file_name)) {
    src.initFile().setBackingFileName(str_to_data(backing_file_name));
  } else if (should_copy_mmap_region(km, stat) &&
             files_assumed_immutable.find(make_pair(
                 stat.st_dev, stat.st_ino)) == files_assumed_immutable.end()) {
    src.setTrace();
  } else {
    // should_copy_mmap_region's heuristics determined it was OK to just map
    // the file here even if it's MAP_SHARED. So try cloning again to avoid
    // the possibility of the file changing between recording and replay.
    if (!try_clone_file(t, km.fsname(), &backing_file_name)) {
      // Try hardlinking file into the trace directory. This will avoid
      // replay failures if the original file is deleted or replaced (but not
      // if it is overwritten in-place). If try_hardlink_file fails it
      // just returns the original file name.
      // A relative backing_file_name is relative to the trace directory.
      backing_file_name = try_hardlink_file(km.fsname());
      files_assumed_immutable.insert(make_pair(stat.st_dev, stat.st_ino));
    }
    src.initFile().setBackingFileName(str_to_data(backing_file_name));
  }

  try {
    auto& mmaps = writer(MMAPS);
    CompressedWriterOutputStream stream(mmaps);
    writePackedMessage(stream, map_msg);
  } catch (...) {
    FATAL() << "Unable to write mmaps";
  }

  ++mmap_count;
  return src.isTrace() ? RECORD_IN_TRACE : DONT_RECORD_IN_TRACE;
}

void TraceWriter::write_mapped_region_to_alternative_stream(
    CompressedWriter& mmaps, const MappedData& data, const KernelMapping& km) {
  MallocMessageBuilder map_msg;
  trace::MMap::Builder map = map_msg.initRoot<trace::MMap>();

  map.setFrameTime(data.time);
  map.setStart(km.start().as_int());
  map.setEnd(km.end().as_int());
  map.setFsname(str_to_data(km.fsname()));
  map.setDevice(km.device());
  map.setInode(km.inode());
  map.setProt(km.prot());
  map.setFlags(km.flags());
  map.setFileOffsetBytes(km.file_offset_bytes());
  map.setStatSize(data.file_size_bytes);
  auto src = map.getSource();
  switch (data.source) {
    case TraceReader::SOURCE_ZERO:
      src.setZero();
      break;
    case TraceReader::SOURCE_TRACE:
      src.setTrace();
      break;
    case TraceReader::SOURCE_FILE:
      src.initFile().setBackingFileName(str_to_data(data.file_name));
      break;
    default:
      FATAL() << "Unknown source type";
      break;
  }

  try {
    CompressedWriterOutputStream stream(mmaps);
    writePackedMessage(stream, map_msg);
  } catch (...) {
    FATAL() << "Unable to write mmaps";
  }
}

KernelMapping TraceReader::read_mapped_region(MappedData* data, bool* found,
                                              ValidateSourceFile validate,
                                              TimeConstraint time_constraint) {
  if (found) {
    *found = false;
  }

  auto& mmaps = reader(MMAPS);
  if (mmaps.at_end()) {
    return KernelMapping();
  }

  if (time_constraint == CURRENT_TIME_ONLY) {
    mmaps.save_state();
  }
  CompressedReaderInputStream stream(mmaps);
  PackedMessageReader map_msg(stream);
  trace::MMap::Reader map = map_msg.getRoot<trace::MMap>();
  if (time_constraint == CURRENT_TIME_ONLY) {
    if (map.getFrameTime() != global_time) {
      mmaps.restore_state();
      return KernelMapping();
    }
    mmaps.discard_state();
  }

  if (data) {
    data->time = map.getFrameTime();
    if (data->time <= 0) {
      FATAL() << "Invalid frameTime";
    }
    data->data_offset_bytes = 0;
    data->file_size_bytes = map.getStatSize();
    auto src = map.getSource();
    switch (src.which()) {
      case trace::MMap::Source::Which::ZERO:
        data->source = SOURCE_ZERO;
        break;
      case trace::MMap::Source::Which::TRACE:
        data->source = SOURCE_TRACE;
        break;
      case trace::MMap::Source::Which::FILE: {
        data->source = SOURCE_FILE;
        static const string clone_prefix("mmap_clone_");
        string backing_file_name = data_to_str(src.getFile().getBackingFileName());
        bool is_clone =
            backing_file_name.substr(0, clone_prefix.size()) == clone_prefix;
        if (backing_file_name[0] != '/') {
          backing_file_name = dir() + "/" + backing_file_name;
        }
        uint32_t uid = map.getStatUid();
        uint32_t gid = map.getStatGid();
        uint32_t mode = map.getStatMode();
        int64_t mtime = map.getStatMTime();
        int64_t size = map.getStatSize();
        if (size < 0) {
          FATAL() << "Invalid statSize";
        }
        bool has_stat_buf = mode != 0 || uid != 0 || gid != 0 || mtime != 0;
        if (!is_clone && validate == VALIDATE && has_stat_buf) {
          struct stat backing_stat;
          if (stat(backing_file_name.c_str(), &backing_stat)) {
            FATAL() << "Failed to stat " << backing_file_name
                    << ": replay is impossible";
          }
          if (backing_stat.st_ino != map.getInode() ||
              backing_stat.st_mode != mode || backing_stat.st_uid != uid ||
              backing_stat.st_gid != gid ||
              backing_stat.st_size != size ||
              backing_stat.st_mtime != mtime) {
            LOG(error) << "Metadata of " << data_to_str(map.getFsname())
                       << " changed: replay divergence likely, but continuing "
                          "anyway. inode: "
                       << backing_stat.st_ino << "/" << map.getInode()
                       << "; mode: " << backing_stat.st_mode << "/" << mode
                       << "; uid: " << backing_stat.st_uid << "/" << uid
                       << "; gid: " << backing_stat.st_gid << "/" << gid
                       << "; size: " << backing_stat.st_size << "/" << size
                       << "; mtime: " << backing_stat.st_mtime << "/" << mtime;
          }
        }
        data->file_name = backing_file_name;
        int64_t file_offset_bytes = map.getFileOffsetBytes();
        if (file_offset_bytes < 0) {
          FATAL() << "Invalid fileOffsetBytes";
        }
        data->data_offset_bytes = file_offset_bytes;
        break;
      }
      default:
        FATAL() << "Unknown mapping source";
        break;
    }
  }
  if (found) {
    *found = true;
  }
  return KernelMapping(map.getStart(), map.getEnd(), data_to_str(map.getFsname()),
                       map.getDevice(), map.getInode(), map.getProt(),
                       map.getFlags(), map.getFileOffsetBytes());
}

void TraceWriter::write_raw(pid_t rec_tid, const void* d, size_t len,
                            remote_ptr<void> addr) {
  auto& data = writer(RAW_DATA);
  auto& data_header = writer(RAW_DATA_HEADER);
  data_header << global_time << rec_tid << addr.as_int() << len;
  data.write(d, len);
}

TraceReader::RawData TraceReader::read_raw_data() {
  auto& data = reader(RAW_DATA);
  auto& data_header = reader(RAW_DATA_HEADER);
  FrameTime time;
  RawData d;
  size_t num_bytes;
  data_header >> time >> d.rec_tid >> d.addr >> num_bytes;
  assert(time == global_time);
  d.data.resize(num_bytes);
  data.read((char*)d.data.data(), num_bytes);
  return d;
}

bool TraceReader::read_raw_data_for_frame(const TraceFrame& frame, RawData& d) {
  auto& data_header = reader(RAW_DATA_HEADER);
  if (data_header.at_end()) {
    return false;
  }
  FrameTime time;
  data_header.save_state();
  data_header >> time;
  data_header.restore_state();
  assert(time >= frame.time());
  if (time > frame.time()) {
    return false;
  }
  d = read_raw_data();
  return true;
}

void TraceWriter::write_generic(const void* d, size_t len) {
  auto& generic = writer(GENERIC);
  generic << global_time << len;
  generic.write(d, len);
}

void TraceReader::read_generic(vector<uint8_t>& out) {
  auto& generic = reader(GENERIC);
  FrameTime time;
  size_t num_bytes;
  generic >> time >> num_bytes;
  assert(time == global_time);
  out.resize(num_bytes);
  generic.read((char*)out.data(), num_bytes);
}

bool TraceReader::read_generic_for_frame(const TraceFrame& frame,
                                         vector<uint8_t>& out) {
  auto& generic = reader(GENERIC);
  if (generic.at_end()) {
    return false;
  }
  FrameTime time;
  generic.save_state();
  generic >> time;
  generic.restore_state();
  assert(time >= frame.time());
  if (time > frame.time()) {
    return false;
  }
  read_generic(out);
  return true;
}

void TraceWriter::close() {
  for (auto& w : writers) {
    w->close();
  }
}

static string make_trace_dir(const string& exe_path) {
  ensure_default_rr_trace_dir();

  // Find a unique trace directory name.
  int nonce = 0;
  int ret;
  string dir;
  do {
    stringstream ss;
    ss << trace_save_dir() << "/" << basename(exe_path.c_str()) << "-"
       << nonce++;
    dir = ss.str();
    ret = mkdir(dir.c_str(), S_IRWXU | S_IRWXG);
  } while (ret && EEXIST == errno);

  if (ret) {
    FATAL() << "Unable to create trace directory `" << dir << "'";
  }

  return dir;
}

#define STR_HELPER(x) #x
#define STR(x) STR_HELPER(x)

TraceWriter::TraceWriter(const std::string& file_name, int bind_to_cpu,
                         bool has_cpuid_faulting)
    : TraceStream(make_trace_dir(file_name),
                  // Somewhat arbitrarily start the
                  // global time from 1.
                  1),
      mmap_count(0),
      supports_file_data_cloning_(false) {
  this->bind_to_cpu = bind_to_cpu;

  for (Substream s = SUBSTREAM_FIRST; s < SUBSTREAM_COUNT; ++s) {
    writers[s] = unique_ptr<CompressedWriter>(new CompressedWriter(
        path(s), substream(s).block_size, substream(s).threads));
  }

  string ver_path = version_path();
  ScopedFd version_fd(ver_path.c_str(), O_RDWR | O_CREAT, 0600);
  if (!version_fd.is_open()) {
    FATAL() << "Unable to create " << ver_path;
  }
  static const char buf[] = STR(TRACE_VERSION) "\n";
  if (write(version_fd, buf, sizeof(buf) - 1) != (ssize_t)sizeof(buf) - 1) {
    FATAL() << "Unable to write " << ver_path;
  }

  // We are now bound to the selected CPU (if any), so collect CPUID records
  // (which depend on the bound CPU number).
  vector<CPUIDRecord> cpuid_records = all_cpuid_records();

  MallocMessageBuilder header_msg;
  trace::Header::Builder header = header_msg.initRoot<trace::Header>();
  header.setBindToCpu(bind_to_cpu);
  header.setHasCpuidFaulting(has_cpuid_faulting);
  header.setCpuidRecords(
      Data::Reader(reinterpret_cast<const uint8_t*>(cpuid_records.data()),
                   cpuid_records.size() * sizeof(CPUIDRecord)));
  // Add a random UUID to the trace metadata. This lets tools identify a trace
  // easily.
  uint8_t uuid[16];
  good_random(uuid, sizeof(uuid));
  header.setUuid(Data::Reader(uuid, sizeof(uuid)));
  try {
    writePackedMessageToFd(version_fd, header_msg);
  } catch (...) {
    FATAL() << "Unable to write " << ver_path;
  }

  // Test if file data cloning is supported
  string version_clone_path = trace_dir + "/tmp_clone";
  ScopedFd version_clone_fd(version_clone_path.c_str(), O_WRONLY | O_CREAT,
                            0600);
  if (!version_clone_fd.is_open()) {
    FATAL() << "Unable to create " << version_clone_path;
  }
  btrfs_ioctl_clone_range_args clone_args;
  clone_args.src_fd = version_fd;
  clone_args.src_offset = 0;
  off_t offset = lseek(version_fd, 0, SEEK_END);
  if (offset <= 0) {
    FATAL() << "Unable to lseek " << ver_path;
  }
  clone_args.src_length = offset;
  clone_args.dest_offset = 0;
  if (ioctl(version_clone_fd, BTRFS_IOC_CLONE_RANGE, &clone_args) == 0) {
    supports_file_data_cloning_ = true;
  }
  unlink(version_clone_path.c_str());

  if (!probably_not_interactive(STDOUT_FILENO)) {
    printf("rr: Saving execution to trace directory `%s'.\n",
           trace_dir.c_str());
  }
}

void TraceWriter::make_latest_trace() {
  string link_name = latest_trace_symlink();
  // Try to update the symlink to |this|.  We only try attempt
  // to set the symlink once.  If the link is re-created after
  // we |unlink()| it, then another rr process is racing with us
  // and it "won".  The link is then valid and points at some
  // very-recent trace, so that's good enough.
  unlink(link_name.c_str());
  int ret = symlink(trace_dir.c_str(), link_name.c_str());
  if (ret < 0 && errno != EEXIST) {
    FATAL() << "Failed to update symlink `" << link_name << "' to `"
            << trace_dir << "'.";
  }
}

TraceFrame TraceReader::peek_frame() {
  auto& events = reader(EVENTS);
  events.save_state();
  auto saved_time = global_time;
  TraceFrame frame;
  if (!at_end()) {
    frame = read_frame();
  }
  events.restore_state();
  global_time = saved_time;
  return frame;
}

void TraceReader::rewind() {
  for (Substream s = SUBSTREAM_FIRST; s < SUBSTREAM_COUNT; ++s) {
    reader(s).rewind();
  }
  global_time = 0;
  assert(good());
}

TraceReader::TraceReader(const string& dir)
    : TraceStream(dir.empty() ? latest_trace_symlink() : dir, 1) {
  for (Substream s = SUBSTREAM_FIRST; s < SUBSTREAM_COUNT; ++s) {
    readers[s] = unique_ptr<CompressedReader>(new CompressedReader(path(s)));
  }

  string path = version_path();
  ScopedFd version_fd(path.c_str(), O_RDONLY);
  if (!version_fd.is_open()) {
    if (errno == ENOENT) {
      fprintf(stderr, "\n"
                      "rr: error: Trace version file `%s' not found. There is "
                      "probably no trace there.\n"
                      "\n",
              path.c_str());
    } else {
      fprintf(stderr, "\n"
                      "rr: error: Trace version file `%s' not readable.\n"
                      "\n",
              path.c_str());
    }
    exit(EX_DATAERR);
  }
  string version_str;
  while (true) {
    char ch;
    ssize_t ret = read(version_fd, &ch, 1);
    if (ret <= 0) {
      FATAL() << "Can't read version file " << path;
    }
    if (ch == '\n') {
      break;
    }
    version_str += ch;
  }
  char* end_ptr;
  long int version = strtol(version_str.c_str(), &end_ptr, 10);
  if (*end_ptr != 0) {
    FATAL() << "Invalid version: " << version_str;
  }
  if (TRACE_VERSION != version) {
    fprintf(stderr, "\n"
                    "rr: error: Recorded trace `%s' has an incompatible "
                    "version %ld; expected\n"
                    "           %d.  Did you record `%s' with an older version "
                    "of rr?  If so,\n"
                    "           you'll need to replay `%s' with that older "
                    "version.  Otherwise,\n"
                    "           your trace is likely corrupted.\n"
                    "\n",
            path.c_str(), version, TRACE_VERSION, path.c_str(), path.c_str());
    exit(EX_DATAERR);
  }

  PackedFdMessageReader header_msg(version_fd);

  trace::Header::Reader header = header_msg.getRoot<trace::Header>();
  bind_to_cpu = header.getBindToCpu();
  trace_uses_cpuid_faulting = header.getHasCpuidFaulting();
  Data::Reader cpuid_records_bytes = header.getCpuidRecords();
  size_t len = cpuid_records_bytes.size() / sizeof(CPUIDRecord);
  assert(cpuid_records_bytes.size() == len * sizeof(CPUIDRecord));
  cpuid_records_.resize(len);
  memcpy(cpuid_records_.data(), cpuid_records_bytes.begin(),
         len * sizeof(CPUIDRecord));

  // Set the global time at 0, so that when we tick it for the first
  // event, it matches the initial global time at recording, 1.
  global_time = 0;
}

/**
 * Create a copy of this stream that has exactly the same
 * state as 'other', but for which mutations of this
 * clone won't affect the state of 'other' (and vice versa).
 */
TraceReader::TraceReader(const TraceReader& other)
    : TraceStream(other.dir(), other.time()) {
  for (Substream s = SUBSTREAM_FIRST; s < SUBSTREAM_COUNT; ++s) {
    readers[s] =
        unique_ptr<CompressedReader>(new CompressedReader(other.reader(s)));
  }

  bind_to_cpu = other.bind_to_cpu;
  trace_uses_cpuid_faulting = other.trace_uses_cpuid_faulting;
  cpuid_records_ = other.cpuid_records_;
}

TraceReader::~TraceReader() {}

uint64_t TraceReader::uncompressed_bytes() const {
  uint64_t total = 0;
  for (Substream s = SUBSTREAM_FIRST; s < SUBSTREAM_COUNT; ++s) {
    total += reader(s).uncompressed_bytes();
  }
  return total;
}

uint64_t TraceReader::compressed_bytes() const {
  uint64_t total = 0;
  for (Substream s = SUBSTREAM_FIRST; s < SUBSTREAM_COUNT; ++s) {
    total += reader(s).compressed_bytes();
  }
  return total;
}

} // namespace rr
