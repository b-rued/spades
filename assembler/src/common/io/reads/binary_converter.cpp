//***************************************************************************
//* Copyright (c) 2019 Saint Petersburg State University
//* All Rights Reserved
//* See file LICENSE for details.
//***************************************************************************

#include "binary_converter.hpp"

#include "read_stream.hpp"
#include "single_read.hpp"
#include "paired_read.hpp"
#include "orientation.hpp"

#include "library/library.hpp"
#include "utils/logger/logger.hpp"
#include "utils/verify.hpp"

#include "threadpool/threadpool.hpp"

namespace io {

template<class Read>
class ReadBinaryWriter {
    bool rc_;

public:
    ReadBinaryWriter(bool rc = false)
            : rc_(rc) {}

    bool Write(std::ostream& file, const Read& r) const {
        return r.BinWrite(file, rc_);
    }
};

template<class Read>
class PairedReadBinaryWriter {
    bool rc1_;
    bool rc2_;

public:
    PairedReadBinaryWriter(LibraryOrientation orientation = LibraryOrientation::Undefined) {
        std::tie(rc1_, rc2_) = GetRCFlags(orientation);
    }

    bool Write(std::ostream& file, const Read& r) const {
        return r.BinWrite(file, rc1_, rc2_);
    }
};

template<class Writer, class Read>
ReadStreamStat BinaryWriter::ToBinary(const Writer &writer, io::ReadStream<Read> &stream,
                                      ThreadPool::ThreadPool *pool) {
    std::vector<Read> buf, flush_buf;
    DEBUG("Reserving a buffer for " << BUF_SIZE << " reads");
    buf.resize(BUF_SIZE); flush_buf.resize(BUF_SIZE);

    // Reserve space for stats
    ReadStreamStat read_stats;
    read_stats.write(*file_ds_);

    size_t rest = 1;
    std::future<void> flush_task;
    auto flush_buffer = [&](size_t sz) {
        // Wait for completion of the current flush task
        if (flush_task.valid())
            flush_task.wait();

        std::swap(buf, flush_buf);

        auto flush_job = [&, sz] {
            for (size_t i = 0; i < sz; ++i) {
                const Read &read = flush_buf[i];
                if (!--rest) {
                    auto offset = (size_t)file_ds_->tellp();
                    offset_ds_->write(reinterpret_cast<const char*>(&offset), sizeof(offset));
                    rest = CHUNK;
                }
                writer.Write(*file_ds_, read);
            }
        };

        if (pool)
            flush_task = pool->run(flush_job);
        else
            flush_job();
    };

    size_t read_count = 0, buf_size = 0;
    Read read;
    while (!stream.eof()) {
        stream >> buf[buf_size];
        read_stats.increase(buf[buf_size]);

        VERBOSE_POWER(++read_count, " reads processed");

        if (++buf_size == BUF_SIZE) {
            flush_buffer(buf_size);
            buf_size = 0;
        }
    }
    flush_buffer(buf_size); // Write leftovers
    // Wait for completion of the current final task
    if (flush_task.valid())
        flush_task.wait();

    // Rewrite the reserved space with actual stats
    file_ds_->seekp(0);
    read_stats.write(*file_ds_);

    INFO(read_count << " reads written");
    return read_stats;
}

BinaryWriter::BinaryWriter(const std::string &file_name_prefix)
            : file_name_prefix_(file_name_prefix),
              file_ds_(std::make_unique<std::ofstream>(file_name_prefix_ + ".seq", std::ios_base::binary)),
              offset_ds_(std::make_unique<std::ofstream>(file_name_prefix_ + ".off", std::ios_base::binary))
{}

ReadStreamStat BinaryWriter::ToBinary(io::ReadStream<io::SingleReadSeq>& stream,
                                      ThreadPool::ThreadPool *pool) {
    ReadBinaryWriter<io::SingleReadSeq> read_writer;
    return ToBinary(read_writer, stream, pool);
}

ReadStreamStat BinaryWriter::ToBinary(io::ReadStream<io::SingleRead>& stream,
                                      ThreadPool::ThreadPool *pool) {
    ReadBinaryWriter<io::SingleRead> read_writer;
    return ToBinary(read_writer, stream, pool);
}

ReadStreamStat BinaryWriter::ToBinary(io::ReadStream<io::PairedReadSeq>& stream,
                                      LibraryOrientation orientation,
                                      ThreadPool::ThreadPool *pool) {
    PairedReadBinaryWriter<io::PairedReadSeq> read_writer(orientation);
    return ToBinary(read_writer, stream, pool);
}

ReadStreamStat BinaryWriter::ToBinary(io::ReadStream<io::PairedRead>& stream,
                                      LibraryOrientation orientation,
                                      ThreadPool::ThreadPool *pool) {
    PairedReadBinaryWriter<io::PairedRead> read_writer(orientation);
    return ToBinary(read_writer, stream, pool);
}

}
