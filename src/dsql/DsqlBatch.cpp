/*
 *  The contents of this file are subject to the Initial
 *  Developer's Public License Version 1.0 (the "License");
 *  you may not use this file except in compliance with the
 *  License. You may obtain a copy of the License at
 *  http://www.ibphoenix.com/main.nfs?a=ibphoenix&page=ibp_idpl.
 *
 *  Software distributed under the License is distributed AS IS,
 *  WITHOUT WARRANTY OF ANY KIND, either express or implied.
 *  See the License for the specific language governing rights
 *  and limitations under the License.
 *
 *  The Original Code was created by Alexander Peshkov
 *  for the Firebird Open Source RDBMS project.
 *
 *  Copyright (c) 2017 Alexander Peshkov <peshkoff@mail.ru>
 *  and all contributors signed below.
 *
 *  All Rights Reserved.
 *  Contributor(s): ________________________________
 */

#include "firebird.h"

#include "../dsql/DsqlBatch.h"

#include "../jrd/EngineInterface.h"
#include "../jrd/jrd.h"
#include "../jrd/status.h"
#include "../jrd/exe_proto.h"
#include "../dsql/dsql.h"
#include "../dsql/errd_proto.h"
#include "../common/classes/ClumpletReader.h"
#include "../common/classes/auto.h"
#include "../common/classes/fb_string.h"
#include "../common/utils_proto.h"
#include "../common/classes/BatchCompletionState.h"

using namespace Firebird;
using namespace Jrd;

namespace {
	const char* TEMP_NAME = "fb_batch";
	const UCHAR blobParameters[] = {isc_bpb_version1, isc_bpb_type, 1, isc_bpb_type_stream};

	class JTrans : public Firebird::Transliterate
	{
	public:
		JTrans(thread_db* tdbb)
			: m_tdbb(tdbb)
		{ }

		void transliterate(IStatus* status)
		{
			JRD_transliterate(m_tdbb, status);
		}

	private:
		thread_db* m_tdbb;
	};
}

DsqlBatch::DsqlBatch(dsql_req* req, const dsql_msg* /*message*/, IMessageMetadata* inMeta, ClumpletReader& pb)
	: m_request(req),
	  m_batch(NULL),
	  m_meta(inMeta),
	  m_messages(m_request->getPool()),
	  m_blobs(m_request->getPool()),
	  m_blobMap(m_request->getPool()),
	  m_blobMeta(m_request->getPool()),
	  m_messageSize(0),
	  m_alignedMessage(0),
	  m_alignment(0),
	  m_flags(0),
	  m_detailed(DETAILED_LIMIT),
	  m_bufferSize(BUFFER_LIMIT),
	  m_lastBlob(MAX_ULONG),
	  m_setBlobSize(false),
	  m_blobPolicy(IBatch::BLOB_IDS_NONE)
{
	memset(&m_genId, 0, sizeof(m_genId));

	FbLocalStatus st;
	m_messageSize = m_meta->getMessageLength(&st);
	m_alignedMessage = m_meta->getAlignedLength(&st);
	m_alignment = m_meta->getAlignment(&st);
	check(&st);

	if (m_messageSize > RAM_BATCH)		// hops - message does not fit in our buffer
	{
		ERRD_post(Arg::Gds(isc_sqlerr) << Arg::Num(-104) <<
				  Arg::Gds(isc_random) << "Message too long");
	}

	for (pb.rewind(); !pb.isEof(); pb.moveNext())
	{
		UCHAR t = pb.getClumpTag();
		switch(t)
		{
		case IBatch::MULTIERROR:
		case IBatch::RECORD_COUNTS:
			if (pb.getInt())
				m_flags |= (1 << t);
			else
				m_flags &= ~(1 << t);
			break;

		case IBatch::BLOB_IDS:
			m_blobPolicy = pb.getInt();
			switch(m_blobPolicy)
			{
			case IBatch::BLOB_IDS_ENGINE:
			case IBatch::BLOB_IDS_USER:
			case IBatch::BLOB_IDS_STREAM:
				break;
			default:
				m_blobPolicy = IBatch::BLOB_IDS_NONE;
				break;
			}
			break;

		case IBatch::DETAILED_ERRORS:
			m_detailed = pb.getInt();
			if (m_detailed > DETAILED_LIMIT * 4)
				m_detailed = DETAILED_LIMIT * 4;
			break;

		case IBatch::BUFFER_BYTES_SIZE:
			m_bufferSize = pb.getInt();
			if (m_bufferSize > BUFFER_LIMIT * 4)
				m_bufferSize = BUFFER_LIMIT * 4;
			break;
		}
	}

	// parse message to detect blobs
	unsigned fieldsCount = m_meta->getCount(&st);
	check(&st);
	for (unsigned i = 0; i < fieldsCount; ++i)
	{
		unsigned t = m_meta->getType(&st, i);
		check(&st);
		switch(t)
		{
		case SQL_BLOB:
		case SQL_ARRAY:
			{
				BlobMeta bm;
				bm.offset = m_meta->getOffset(&st, i);
				check(&st);
				bm.nullOffset = m_meta->getNullOffset(&st, i);
				check(&st);
				m_blobMeta.push(bm);
			}
			break;
		}
	}

	// allocate data buffers
	m_messages.setBuf(m_bufferSize);
	if (m_blobMeta.hasData())
		m_blobs.setBuf(m_bufferSize);
}


DsqlBatch::~DsqlBatch()
{
	if (m_batch)
		m_batch->resetHandle();
	if (m_request)
		m_request->req_batch = NULL;
}

Attachment* DsqlBatch::getAttachment() const
{
	return m_request->req_dbb->dbb_attachment;
}

void DsqlBatch::setInterfacePtr(JBatch* interfacePtr) throw()
{
	fb_assert(!m_batch);
	m_batch = interfacePtr;
}

DsqlBatch* DsqlBatch::open(thread_db* tdbb, dsql_req* req, IMessageMetadata* inMetadata,
	unsigned parLength, const UCHAR* par)
{
	SET_TDBB(tdbb);
	Jrd::ContextPoolHolder context(tdbb, &req->getPool());

	// Validate cursor or batch being not already open

	if (req->req_cursor)
	{
		ERRD_post(Arg::Gds(isc_sqlerr) << Arg::Num(-502) <<
				  Arg::Gds(isc_dsql_cursor_open_err));
	}

	if (req->req_batch)
	{
		ERRD_post(Arg::Gds(isc_sqlerr) << Arg::Num(-502) <<
				  Arg::Gds(isc_random) << "Request has active batch");
	}

	// Sanity checks before creating batch

	if (!req->req_request)
	{
		ERRD_post(Arg::Gds(isc_sqlerr) << Arg::Num(-504) <<
				  Arg::Gds(isc_unprepared_stmt));
	}

	const DsqlCompiledStatement* statement = req->getStatement();

	if (statement->getFlags() & DsqlCompiledStatement::FLAG_ORPHAN)
	{
		ERRD_post(Arg::Gds(isc_sqlerr) << Arg::Num(-901) <<
		          Arg::Gds(isc_bad_req_handle));
	}

	switch(statement->getType())
	{
		case DsqlCompiledStatement::TYPE_INSERT:
		case DsqlCompiledStatement::TYPE_DELETE:
		case DsqlCompiledStatement::TYPE_UPDATE:
		case DsqlCompiledStatement::TYPE_EXEC_PROCEDURE:
		case DsqlCompiledStatement::TYPE_EXEC_BLOCK:
			break;

		default:
			ERRD_post(Arg::Gds(isc_sqlerr) << Arg::Num(-901) <<
					  Arg::Gds(isc_random) << "Invalid type of statement used in batch");
	}

	const dsql_msg* message = statement->getSendMsg();
	if (! (inMetadata && message && req->parseMetadata(inMetadata, message->msg_parameters)))
	{
		ERRD_post(Arg::Gds(isc_sqlerr) << Arg::Num(-901) <<
				  Arg::Gds(isc_random) << "Statement used in batch must have parameters");
	}

	// Open reader for parameters block

	ClumpletReader pb(ClumpletReader::WideTagged, par, parLength);
	if (pb.getBufferLength() && (pb.getBufferTag() != IBatch::VERSION1))
		ERRD_post(Arg::Gds(isc_random) << "Invalid tag in parameters block");

	// Create batch

	DsqlBatch* b = FB_NEW_POOL(req->getPool()) DsqlBatch(req, message, inMetadata, pb);
	req->req_batch = b;
	return b;
}

void DsqlBatch::add(thread_db* tdbb, ULONG count, const void* inBuffer)
{
	if (!count)
		return;
	m_messages.align(m_alignment);
	m_messages.put(inBuffer, (count - 1) * m_alignedMessage + m_messageSize);
}

void DsqlBatch::blobCheckMeta()
{
	if (!m_blobMeta.hasData())
	{
		ERRD_post(Arg::Gds(isc_sqlerr) << Arg::Num(-104) <<
			Arg::Gds(isc_random) << "There are no blobs in associated statement");
	}
}

void DsqlBatch::blobCheckMode(bool stream, const char* fname)
{
	blobCheckMeta();

	switch(m_blobPolicy)
	{
	case IBatch::BLOB_IDS_ENGINE:
	case IBatch::BLOB_IDS_USER:
		if (!stream)
			return;
		break;
	case IBatch::BLOB_IDS_STREAM:
		if (stream)
			return;
		break;
	}

	ERRD_post(Arg::Gds(isc_sqlerr) << Arg::Num(-104) <<
		Arg::Gds(isc_random) << "This *** call can't be used with current blob policy" <<
		Arg::Gds(isc_random) << fname);
}

void DsqlBatch::blobPrepare()
{
	// Store size of previous blob if it was changed by appendBlobData()
	unsigned blobSize = m_blobs.getSize();
	if (m_setBlobSize)
	{
		blobSize -= (m_lastBlob + SIZEOF_BLOB_HEAD);
		m_blobs.put3(&blobSize, sizeof(blobSize), m_lastBlob + sizeof(ISC_QUAD));
		m_setBlobSize = false;
	}

	// Align blob stream
	m_blobs.align(BLOB_STREAM_ALIGN);
}

void DsqlBatch::addBlob(thread_db* tdbb, ULONG length, const void* inBuffer, ISC_QUAD* blobId)
{
	blobCheckMode(false, "addBlob");
	blobPrepare();

	// Get ready to appendBlobData()
	m_lastBlob = m_blobs.getSize();
	fb_assert(m_lastBlob % BLOB_STREAM_ALIGN == 0);

	// Generate auto blob ID if needed
	if (m_blobPolicy == IBatch::BLOB_IDS_ENGINE)
		genBlobId(blobId);

	// Store header
	m_blobs.put(blobId, sizeof(ISC_QUAD));
	m_blobs.put(&length, sizeof(ULONG));

	// Finally store user data
	m_blobs.put(inBuffer, length);
}

void DsqlBatch::appendBlobData(thread_db* tdbb, ULONG length, const void* inBuffer)
{
	blobCheckMode(false, "appendBlobData");

	if (m_lastBlob == MAX_ULONG)
	{
		ERRD_post(Arg::Gds(isc_sqlerr) << Arg::Num(-104) <<
			Arg::Gds(isc_random) << "appendBlobData() is used to append data to last blob "
									"but no such blob was added to the batch");
	}

	m_setBlobSize = true;
	m_blobs.put(inBuffer, length);
}

void DsqlBatch::addBlobStream(thread_db* tdbb, unsigned length, const void* inBuffer)
{
	// Sanity checks
	if (length == 0)
		return;
	if (length % BLOB_STREAM_ALIGN)
	{
		ERRD_post(Arg::Gds(isc_sqlerr) << Arg::Num(-104) <<
			Arg::Gds(isc_random) << "Portions of data, passed as blob stream, should have size "
				"multiple to the alignment required for blobs");
	}

	blobCheckMode(true, "addBlobStream");
	blobPrepare();

	// We have no idea where is the last blob located in the stream
	m_lastBlob = MAX_ULONG;

	// store stream for further processing
	fb_assert(m_blobs.getSize() % BLOB_STREAM_ALIGN == 0);
	m_blobs.put(inBuffer, length);
}

void DsqlBatch::registerBlob(thread_db* tdbb, const ISC_QUAD* existingBlob, ISC_QUAD* blobId)
{
	blobCheckMeta();

	ISC_QUAD* idPtr = m_blobMap.put(*existingBlob);
	if (!idPtr)
	{
		ERRD_post(Arg::Gds(isc_sqlerr) << Arg::Num(-104) <<
			Arg::Gds(isc_random) << "Repeated BlobId in registerBlob(): is ***");
	}

	*idPtr = *blobId;
}

Firebird::IBatchCompletionState* DsqlBatch::execute(thread_db* tdbb)
{
	// todo - add new trace event here
	// TraceDSQLExecute trace(req_dbb->dbb_attachment, this);

	jrd_tra* transaction = tdbb->getTransaction();

	// execution timer
	thread_db::TimerGuard timerGuard(tdbb, m_request->setupTimer(tdbb), true);

	// sync internal buffers
	if (!m_messages.done())
	{
		ERRD_post(Arg::Gds(isc_sqlerr) << Arg::Num(-104) <<
			  Arg::Gds(isc_random) << "Internal message buffer overflow - batch too big");
	}

	// insert blobs here
	if (m_blobMeta.hasData())
	{
		// This code expects the following to work correctly
		fb_assert(RAM_BATCH % BLOB_STREAM_ALIGN == 0);

		if (!m_blobs.done())
		{
			ERRD_post(Arg::Gds(isc_sqlerr) << Arg::Num(-104) <<
				  Arg::Gds(isc_random) << "Internal BLOB buffer overflow - batch too big");
		}

		ULONG remains;
		UCHAR* data;
		ULONG currentBlobSize = 0;
		ULONG byteCount = 0;
		blb* blob = nullptr;
		try
		{
			while ((remains = m_blobs.get(&data)) > 0)
			{
				while (remains)
				{
					// should we get next blob header
					if (!currentBlobSize)
					{
						// skip alignment data
						ULONG align = byteCount % BLOB_STREAM_ALIGN;
						if (align)
						{
							align = BLOB_STREAM_ALIGN - align;
							data += align;
							byteCount += align;
							remains -= align;
							continue;
						}

						// safety check
						if (remains < SIZEOF_BLOB_HEAD)
						{
							ERRD_post(Arg::Gds(isc_sqlerr) << Arg::Num(-104) <<
								Arg::Gds(isc_random) << "Internal error: useless data remained in batch BLOB buffer");
						}

						// parse blob header
						fb_assert(intptr_t(data) % BLOB_STREAM_ALIGN == 0);
						ISC_QUAD* batchBlobId = reinterpret_cast<ISC_QUAD*>(data);
						ULONG* blobSize = reinterpret_cast<ULONG*>(data + sizeof(ISC_QUAD));
						currentBlobSize = *blobSize;
						data += SIZEOF_BLOB_HEAD;
						byteCount += SIZEOF_BLOB_HEAD;
						remains -= SIZEOF_BLOB_HEAD;

						// create blob
						bid engineBlobId;
						blob = blb::create2(tdbb, transaction, &engineBlobId, sizeof(blobParameters), blobParameters, true);
						registerBlob(tdbb, batchBlobId, reinterpret_cast<ISC_QUAD*>(&engineBlobId));
					}

					// store data
					ULONG dataSize = currentBlobSize;
					if (dataSize > remains)
						dataSize = remains;
					blob->BLB_put_segment(tdbb, data, dataSize);

					// account data portion
					data += dataSize;
					byteCount += dataSize;
					remains -= dataSize;
					currentBlobSize -= dataSize;
					if (!currentBlobSize)
					{
						blob->BLB_close(tdbb);
						blob = nullptr;
					}
				}
				m_blobs.remained(0);
			}

			fb_assert(!blob);
			if (blob)
				blob->BLB_cancel(tdbb);
		}
		catch(const Exception&)
		{
			if (blob)
				blob->BLB_cancel(tdbb);
			throw;
		}
	}

	// execute request
	m_request->req_transaction = transaction;
	jrd_req* req = m_request->req_request;
	fb_assert(req);

	// prepare completion interface
	AutoPtr<BatchCompletionState, SimpleDispose<BatchCompletionState> > completionState
		(FB_NEW BatchCompletionState(m_flags & (1 << IBatch::RECORD_COUNTS), m_detailed));
	AutoSetRestore<bool> batchFlag(&req->req_batch, true);
	const dsql_msg* message = m_request->getStatement()->getSendMsg();
	bool startRequest = true;

	ULONG remains;
	UCHAR* data;
	while ((remains = m_messages.get(&data)) > 0)
	{
		if (remains < m_messageSize)
		{
			ERRD_post(Arg::Gds(isc_sqlerr) << Arg::Num(-104) <<
				Arg::Gds(isc_random) << "Internal error: useless data remained in batch buffer");
		}

		while (remains >= m_messageSize)
		{
			if (startRequest)
			{
				DEB_BATCH(fprintf(stderr, "\n\n+++ Unwind\n\n"));
				EXE_unwind(tdbb, req);
				DEB_BATCH(fprintf(stderr, "\n\n+++ Start\n\n"));
				EXE_start(tdbb, req, transaction);
				startRequest = false;
			}

			// skip alignment data
			UCHAR* alignedData = FB_ALIGN(data, m_alignment);
			if (alignedData != data)
			{
				remains -= (alignedData - data);
				data = alignedData;
				continue;
			}

			// translate blob IDs
			fb_assert(intptr_t(data) % m_alignment == 0);
			for (unsigned i = 0; i < m_blobMeta.getCount(); ++i)
			{
				const SSHORT* nullFlag = reinterpret_cast<const SSHORT*>(&data[m_blobMeta[i].nullOffset]);
				if (*nullFlag)
					continue;

				ISC_QUAD* id = reinterpret_cast<ISC_QUAD*>(&data[m_blobMeta[i].offset]);
				ISC_QUAD newId;
				if (!m_blobMap.get(*id, newId))
				{
					ERRD_post(Arg::Gds(isc_sqlerr) << Arg::Num(-104) <<
						Arg::Gds(isc_random) << "Unknown blob ID in the message: is ***" <<
						Arg::Gds(isc_random) << Arg::Num(id->gds_quad_high) <<
						Arg::Gds(isc_random) << Arg::Num(id->gds_quad_low));
				}

				m_blobMap.remove(*id);
				*id = newId;
			}

			// map message to internal engine format
			m_request->mapInOut(tdbb, false, message, m_meta, NULL, data);
			data += m_messageSize;
			remains -= m_messageSize;

			UCHAR* msgBuffer = m_request->req_msg_buffers[message->msg_buffer_number];
			DEB_BATCH(fprintf(stderr, "\n\n+++ Send\n\n"));
			try
			{
				ULONG before = req->req_records_inserted + req->req_records_updated +
					req->req_records_deleted;
				EXE_send(tdbb, req, message->msg_number, message->msg_length, msgBuffer);
				ULONG after = req->req_records_inserted + req->req_records_updated +
					req->req_records_deleted;
				completionState->regUpdate(after - before);
			}
			catch (const Exception& ex)
			{
				FbLocalStatus status;
				ex.stuffException(&status);
				tdbb->tdbb_status_vector->init();

				JTrans jtr(tdbb);
				completionState->regError(&status, &jtr);
				if (!(m_flags & (1 << IBatch::MULTIERROR)))
				{
					cancel(tdbb);
					remains = 0;
					break;
				}
				startRequest = true;
			}
		}

		UCHAR* alignedData = FB_ALIGN(data, m_alignment);
		m_messages.remained(remains, alignedData - data);
	}

	// reset to initial state
	cancel(tdbb);

	return completionState.release();
}

void DsqlBatch::cancel(thread_db* tdbb)
{
	m_messages.clear();
	if (m_blobMeta.hasData())
	{
		m_blobs.clear();
		m_setBlobSize = false;
		m_lastBlob = MAX_ULONG;
		memset(&m_genId, 0, sizeof(m_genId));
		m_blobMap.clear();
	}
}

void DsqlBatch::genBlobId(ISC_QUAD* blobId)
{
	if (++m_genId.gds_quad_low == 0)
		++m_genId.gds_quad_high;
	memcpy(blobId, &m_genId, sizeof(m_genId));
}

void DsqlBatch::DataCache::setBuf(ULONG size)
{
	m_limit = size;

	// create ram cache
	fb_assert(!m_cache);
	m_cache = FB_NEW_POOL(getPool()) Cache;
}

void DsqlBatch::DataCache::put3(const void* data, ULONG dataSize, ULONG offset)
{
	// This assertion guarantees that data always fits as a whole into m_cache or m_space,
	// never placed half in one storage, half - in another.
	fb_assert((DsqlBatch::RAM_BATCH % dataSize == 0) && (offset % dataSize == 0));

	if (offset >= m_used)
	{
		// data in cache
		UCHAR* to = m_cache->begin();
		to += (offset - m_used);
		fb_assert(to < m_cache->end());
		memcpy(to, data, dataSize);
	}
	else
	{
		const FB_UINT64 writtenBytes = m_space->write(offset, data, dataSize);
		fb_assert(writtenBytes == dataSize);
	}
}

void DsqlBatch::DataCache::put(const void* d, ULONG dataSize)
{
	if (m_used + (m_cache ? m_cache->getCount() : 0) + dataSize > m_limit)
		ERRD_post(Arg::Gds(isc_sqlerr) << Arg::Num(-104) <<
			  Arg::Gds(isc_random) << "Internal buffer overflow - batch too big");

	const UCHAR* data = reinterpret_cast<const UCHAR*>(d);

	// Coefficient affecting direct data write to tempspace
	const ULONG K = 4;

	// ensure ram cache presence
	fb_assert(m_cache);

	// swap to secondary cache if needed
	if (m_cache->getCount() + dataSize > m_cache->getCapacity())
	{
		// store data in the end of ram cache if needed
		// avoid copy in case of huge buffer passed
		ULONG delta = m_cache->getCapacity() - m_cache->getCount();
		if (dataSize - delta < m_cache->getCapacity() / K)
		{
			m_cache->append(data, delta);
			data += delta;
			dataSize -= delta;
		}

		// swap ram cache to tempspace
		if (!m_space)
			m_space = FB_NEW_POOL(getPool()) TempSpace(getPool(), TEMP_NAME);
		const FB_UINT64 writtenBytes = m_space->write(m_used, m_cache->begin(), m_cache->getCount());
		fb_assert(writtenBytes == m_cache->getCount());
		m_used += m_cache->getCount();
		m_cache->clear();

		// in a case of huge buffer write directly to tempspace
		if (dataSize > m_cache->getCapacity() / K)
		{
			const FB_UINT64 writtenBytes = m_space->write(m_used, data, dataSize);
			fb_assert(writtenBytes == dataSize);
			m_used += dataSize;
			return;
		}
	}

	m_cache->append(data, dataSize);
}

void DsqlBatch::DataCache::align(ULONG alignment)
{
	ULONG a = getSize() % alignment;
	if (a)
	{
		fb_assert(alignment <= sizeof(SINT64));
		SINT64 zero = 0;
		put(&zero, alignment - a);
	}
}

bool DsqlBatch::DataCache::done()
{
	fb_assert(m_cache);

	if (m_cache->getCount() == 0 && m_used == 0)
		return true;	// false?

	if (m_cache->getCount() && m_used)
	{
		fb_assert(m_space);

		const FB_UINT64 writtenBytes = m_space->write(m_used, m_cache->begin(), m_cache->getCount());
		fb_assert(writtenBytes == m_cache->getCount());
		m_used += m_cache->getCount();
		m_cache->clear();
	}
	return true;
}

ULONG DsqlBatch::DataCache::get(UCHAR** buffer)
{
	if (m_used > m_got)
	{
		// get data from tempspace
		ULONG dlen = m_cache->getCount();
		ULONG delta = m_cache->getCapacity() - dlen;
		if (delta > m_used - m_got)
			delta = m_used - m_got;
		UCHAR* buf = m_cache->getBuffer(dlen + delta);
		buf += dlen;
		const FB_UINT64 readBytes = m_space->read(m_got, buf, delta);
		fb_assert(readBytes == delta);
		m_got += delta;
	}

	if (m_cache->getCount())
	{
		if (m_shift)
			m_cache->removeCount(0, m_shift);

		// return buffer full of data
		*buffer = m_cache->begin();
		fb_assert(intptr_t(*buffer) % FB_ALIGNMENT == 0);
		return m_cache->getCount();
	}

	// no more data
	*buffer = nullptr;
	return 0;
}

void DsqlBatch::DataCache::remained(ULONG size, ULONG alignment)
{
	if (size > alignment)
	{
		size -= alignment;
		alignment = 0;
	}
	else
	{
		alignment -= size;
		size = 0;
	}

	if (!size)
		m_cache->clear();
	else
		m_cache->removeCount(0, m_cache->getCount() - size);

	m_shift = alignment;
}

ULONG DsqlBatch::DataCache::getSize() const
{
	fb_assert(m_cache);
	fb_assert((MAX_ULONG - 1) - m_used > m_cache->getCount());
	return m_used + m_cache->getCount();
}

void DsqlBatch::DataCache::clear()
{
	m_cache->clear();
	if (m_space && m_used)
		m_space->releaseSpace(0, m_used);
	m_used = m_got = 0;
}
