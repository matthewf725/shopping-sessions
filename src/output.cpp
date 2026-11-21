#include "output.hpp"
#include <arrow/io/file.h>
#include <arrow/result.h>
#include <parquet/column_writer.h>
#include <parquet/file_writer.h>
#include <parquet/schema.h>

namespace shop {
namespace pq = parquet;
using Node = pq::schema::NodePtr;
using Type = pq::Type;
using Logical = pq::ConvertedType;

static Node column(const char* name, Type::type type, Logical::type logical = Logical::NONE, bool nullable = false) {
    return pq::schema::PrimitiveNode::Make(name, nullable ? pq::Repetition::OPTIONAL : pq::Repetition::REQUIRED, type, logical);
}
struct OutputFile {
    std::shared_ptr<arrow::io::FileOutputStream> stream;
    std::unique_ptr<pq::ParquetFileWriter> parquet;

    void close() {
        parquet->Close();
        if (!stream->closed()) {
            auto status = stream->Close();
            if (!status.ok()) throw std::runtime_error(status.ToString());
        }
    }
};

static OutputFile writer(const fs::path& path, const std::vector<Node>& fields) {
    auto result = arrow::io::FileOutputStream::Open(path.string());
    if (!result.ok()) throw std::runtime_error(result.status().ToString());
    auto schema = std::static_pointer_cast<pq::schema::GroupNode>(
        pq::schema::GroupNode::Make("data", pq::Repetition::REQUIRED, fields));
    auto props = pq::WriterProperties::Builder().disable_dictionary()->disable_statistics()->build();
    return {*result, pq::ParquetFileWriter::Open(*result, schema, props)};
}

template<class Writer, class T>
static void batch(pq::RowGroupWriter* group, const std::vector<T>& values) {
    static_cast<Writer*>(group->NextColumn())->WriteBatch(values.size(), nullptr, nullptr, values.data());
}
static pq::ByteArray bytes(const std::string& s) {
    return {uint32_t(s.size()), reinterpret_cast<const uint8_t*>(s.data())};
}
void write_users(const fs::path& path, const std::vector<UserDay>& rows, const Keys& users, const Dimensions& d) {
    auto file = writer(path, {column("user_id", Type::BYTE_ARRAY, Logical::UTF8), column("day", Type::INT32, Logical::DATE),
        column("sessions", Type::INT32), column("events", Type::INT32), column("revenue_cents", Type::INT64),
        column("top_category", Type::BYTE_ARRAY, Logical::UTF8, true)});
    constexpr size_t batch_size = 64 * 1024;
    for (size_t at = 0; at < rows.size(); at += batch_size) {
        size_t n = std::min(batch_size, rows.size() - at);
        std::vector<std::string> names;
        names.reserve(n);
        std::vector<pq::ByteArray> ids(n), cats;
        std::vector<int32_t> days(n), sessions(n), events(n);
        std::vector<int64_t> revenue(n);
        std::vector<int16_t> present(n);
        Key last = 0;
        for (size_t j = 0; j < n; ++j) {
            const auto& row = rows[at + j];
            if (row.person != last) { names.push_back(users.decode(row.person)); last = row.person; }
            ids[j] = bytes(names.back());
            days[j] = row.day;
            sessions[j] = row.sessions;
            events[j] = row.events;
            revenue[j] = row.revenue;
            present[j] = row.top != 0;
            if (row.top) cats.push_back(bytes(d.categories[row.top - 1]));
        }
        auto* group = file.parquet->AppendRowGroup();
        batch<pq::ByteArrayWriter>(group, ids);
        batch<pq::Int32Writer>(group, days);
        batch<pq::Int32Writer>(group, sessions);
        batch<pq::Int32Writer>(group, events);
        batch<pq::Int64Writer>(group, revenue);
        static_cast<pq::ByteArrayWriter*>(group->NextColumn())->WriteBatch(n, present.data(), nullptr, cats.data());
        group->Close();
    }
    file.close();
}
void write_campaigns(const fs::path& path, const CampaignDays& totals, const Dimensions& d) {
    auto file = writer(path, {column("campaign_id", Type::BYTE_ARRAY, Logical::UTF8), column("channel", Type::BYTE_ARRAY, Logical::UTF8),
        column("day", Type::INT32, Logical::DATE), column("sessions", Type::INT64), column("purchasing_sessions", Type::INT64),
        column("revenue_cents", Type::INT64), column("assisted_sessions", Type::INT64)});
    std::vector<pq::ByteArray> ids, channels;
    std::vector<int32_t> days;
    std::vector<int64_t> sessions, purchasing, revenue, assists;
    totals.each([&](uint32_t c, int32_t day, const Totals& t) {
        ids.push_back(bytes(d.names[c].id));
        channels.push_back(bytes(d.names[c].channel));
        days.push_back(day);
        sessions.push_back(t.sessions);
        purchasing.push_back(t.purchasing);
        revenue.push_back(t.revenue);
        assists.push_back(t.assists);
    });
    if (!days.empty()) {
        auto* group = file.parquet->AppendRowGroup();
        batch<pq::ByteArrayWriter>(group, ids);
        batch<pq::ByteArrayWriter>(group, channels);
        batch<pq::Int32Writer>(group, days);
        batch<pq::Int64Writer>(group, sessions);
        batch<pq::Int64Writer>(group, purchasing);
        batch<pq::Int64Writer>(group, revenue);
        batch<pq::Int64Writer>(group, assists);
        group->Close();
    }
    file.close();
}
} // namespace shop
