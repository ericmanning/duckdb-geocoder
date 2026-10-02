#include "us_geocoder_zip.hpp"

#include "duckdb/common/file_system.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "miniz.hpp"

#include <algorithm>
#include <cstring>

namespace duckdb {
namespace us_geocoder {

// DuckDB wraps miniz in namespace duckdb_miniz; unqualified calls do not
// compile. DuckDB's own miniz_wrapper.hpp qualifies each call the same way.
using namespace duckdb_miniz;

static std::string ToLowerAscii(const std::string &s) {
	std::string out = s;
	for (size_t i = 0; i < out.size(); ++i) {
		out[i] = static_cast<char>(::tolower(static_cast<unsigned char>(out[i])));
	}
	return out;
}

// Strip any directory component an archive entry may carry.
static std::string BaseName(const std::string &path) {
	auto pos = path.find_last_of("/\\");
	return pos == std::string::npos ? path : path.substr(pos + 1);
}

std::vector<ZipEntry> ExtractZipEntries(FileSystem &fs, const std::string &zip_path,
                                        const std::string &dest_dir,
                                        const std::vector<std::string> &wanted) {
	// Read the whole archive into memory. County zips are ~1 MB; the largest
	// 1992 state's OtherFiles.zip is a few MB. MINIZ_NO_STDIO is defined in
	// DuckDB's miniz build, so only the *_mem APIs exist anyway.
	if (!fs.FileExists(zip_path)) {
		throw IOException("us_geocoder_unzip: archive not found: %s", zip_path);
	}
	auto handle = fs.OpenFile(zip_path, FileFlags::FILE_FLAGS_READ);
	auto size = fs.GetFileSize(*handle);
	if (size <= 0) {
		throw IOException("us_geocoder_unzip: archive is empty: %s", zip_path);
	}
	std::vector<char> buffer(static_cast<size_t>(size));
	fs.Read(*handle, buffer.data(), size);
	handle.reset();

	mz_zip_archive zip;
	memset(&zip, 0, sizeof(zip));
	if (!mz_zip_reader_init_mem(&zip, buffer.data(), buffer.size(), 0)) {
		throw IOException("us_geocoder_unzip: not a readable zip archive: %s", zip_path);
	}

	std::vector<std::string> wanted_lower;
	for (size_t i = 0; i < wanted.size(); ++i) {
		wanted_lower.push_back(ToLowerAscii(wanted[i]));
	}

	fs.CreateDirectory(dest_dir);

	std::vector<ZipEntry> written;
	const mz_uint n = mz_zip_reader_get_num_files(&zip);
	for (mz_uint i = 0; i < n; ++i) {
		mz_zip_archive_file_stat stat;
		if (!mz_zip_reader_file_stat(&zip, i, &stat)) {
			continue;
		}
		if (mz_zip_reader_is_file_a_directory(&zip, i)) {
			continue;
		}
		const std::string base = BaseName(stat.m_filename);
		// Match case-insensitively ourselves rather than trusting
		// mz_zip_reader_locate_file's flag-dependent case behaviour.
		// 1992 archives store entries in uppercase.
		if (!wanted_lower.empty()) {
			const std::string base_lower = ToLowerAscii(base);
			if (std::find(wanted_lower.begin(), wanted_lower.end(), base_lower) == wanted_lower.end()) {
				continue;
			}
		}
		size_t out_size = 0;
		void *bytes = mz_zip_reader_extract_to_heap(&zip, i, &out_size, 0);
		if (!bytes) {
			mz_zip_reader_end(&zip);
			throw IOException("us_geocoder_unzip: failed to inflate entry %s from %s", base, zip_path);
		}
		const std::string out_path = fs.JoinPath(dest_dir, base);
		{
			// FILE_FLAGS_FILE_CREATE_NEW maps to O_CREAT|O_TRUNC on POSIX
			// (local_file_system.cpp) and CREATE_ALWAYS on Windows — i.e.
			// create-or-truncate/overwrite semantics, not throw-on-exists.
			// That's exactly what's needed here: a loader restart/retry
			// re-runs extraction into the same temp directory, and a
			// shorter replacement write must not leave stale trailing
			// bytes from a longer previous file.
			auto out = fs.OpenFile(out_path, FileFlags::FILE_FLAGS_WRITE | FileFlags::FILE_FLAGS_FILE_CREATE_NEW);
			if (out_size > 0) {
				fs.Write(*out, bytes, static_cast<int64_t>(out_size));
			}
		}
		mz_free(bytes);
		ZipEntry e;
		e.name = base;
		e.bytes = static_cast<int64_t>(out_size);
		written.push_back(e);
	}
	mz_zip_reader_end(&zip);
	return written;
}

// ---------------------------------------------------------------------
// us_geocoder_unzip table function
// ---------------------------------------------------------------------

struct UnzipBindData : public FunctionData {
	std::string zip_path;
	std::string dest_dir;
	unique_ptr<FunctionData> Copy() const override {
		auto r = make_uniq<UnzipBindData>();
		r->zip_path = zip_path;
		r->dest_dir = dest_dir;
		return std::move(r);
	}
	bool Equals(const FunctionData &other) const override {
		auto &o = other.Cast<UnzipBindData>();
		return zip_path == o.zip_path && dest_dir == o.dest_dir;
	}
};

struct UnzipGlobalState : public GlobalTableFunctionState {
	std::vector<ZipEntry> rows;
	idx_t offset = 0;
	bool ran = false;
	static unique_ptr<GlobalTableFunctionState> Init(ClientContext &, TableFunctionInitInput &) {
		return make_uniq<UnzipGlobalState>();
	}
};

static unique_ptr<FunctionData> UnzipBind(ClientContext &, TableFunctionBindInput &input,
                                          vector<LogicalType> &return_types, vector<string> &names) {
	auto result = make_uniq<UnzipBindData>();
	result->zip_path = StringValue::Get(input.inputs[0]);
	result->dest_dir = StringValue::Get(input.inputs[1]);
	return_types.push_back(LogicalType::VARCHAR);
	names.emplace_back("entry");
	return_types.push_back(LogicalType::BIGINT);
	names.emplace_back("bytes");
	return std::move(result);
}

static void UnzipExecute(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind = data_p.bind_data->Cast<UnzipBindData>();
	auto &gstate = data_p.global_state->Cast<UnzipGlobalState>();
	if (!gstate.ran) {
		auto &fs = FileSystem::GetFileSystem(context);
		gstate.rows = ExtractZipEntries(fs, bind.zip_path, bind.dest_dir, {});
		gstate.ran = true;
	}
	idx_t count = 0;
	while (gstate.offset < gstate.rows.size() && count < STANDARD_VECTOR_SIZE) {
		output.SetValue(0, count, Value(gstate.rows[gstate.offset].name));
		output.SetValue(1, count, Value::BIGINT(gstate.rows[gstate.offset].bytes));
		++gstate.offset;
		++count;
	}
	output.SetCardinality(count);
}

void RegisterZipFunctions(ExtensionLoader &loader) {
	TableFunction fn("us_geocoder_unzip", {LogicalType::VARCHAR, LogicalType::VARCHAR}, UnzipExecute, UnzipBind,
	                 UnzipGlobalState::Init);
	loader.RegisterFunction(fn);
}

} // namespace us_geocoder
} // namespace duckdb
