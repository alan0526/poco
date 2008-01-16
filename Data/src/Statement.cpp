//
// Statement.cpp
//
// $Id: //poco/Main/Data/src/Statement.cpp#11 $
//
// Library: Data
// Package: DataCore
// Module:  Statement
//
// Copyright (c) 2006, Applied Informatics Software Engineering GmbH.
// and Contributors.
//
// Permission is hereby granted, free of charge, to any person or organization
// obtaining a copy of the software and accompanying documentation covered by
// this license (the "Software") to use, reproduce, display, distribute,
// execute, and transmit the Software, and to prepare derivative works of the
// Software, and to permit third-parties to whom the Software is furnished to
// do so, all subject to the following:
// 
// The copyright notices in the Software and this entire statement, including
// the above license grant, this restriction and the following disclaimer,
// must be included in all copies of the Software, in whole or in part, and
// all derivative works of the Software, unless such copies or derivative
// works are solely in the form of machine-executable object code generated by
// a source language processor.
// 
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE, TITLE AND NON-INFRINGEMENT. IN NO EVENT
// SHALL THE COPYRIGHT HOLDERS OR ANYONE DISTRIBUTING THE SOFTWARE BE LIABLE
// FOR ANY DAMAGES OR OTHER LIABILITY, WHETHER IN CONTRACT, TORT OR OTHERWISE,
// ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
// DEALINGS IN THE SOFTWARE.
//


#include "Poco/Data/Statement.h"
#include "Poco/Data/DataException.h"
#include "Poco/Data/Extraction.h"
#include "Poco/Data/Session.h"
#include "Poco/Data/Bulk.h"
#include "Poco/Any.h"
#include "Poco/Tuple.h"
#include "Poco/ActiveMethod.h"
#include <algorithm>


namespace Poco {
namespace Data {


Statement::Statement(StatementImpl* pImpl):
	_pImpl(pImpl),
	_async(false)
{
	poco_check_ptr (pImpl);
}


Statement::Statement(Session& session):
	_async(false)
{
	reset(session);
}


Statement::Statement(const Statement& stmt):
	_pImpl(stmt._pImpl),
	_async(stmt._async),
	_pResult(stmt._pResult),
	_pAsyncExec(stmt._pAsyncExec),
	_arguments(stmt._arguments),
	_pRowFormatter(stmt._pRowFormatter)
{
}


Statement::~Statement()
{
}


Statement& Statement::operator = (const Statement& stmt)
{
	Statement tmp(stmt);
	swap(tmp);
	return *this;
}


void Statement::swap(Statement& other)
{
	using std::swap;
	
	swap(_pImpl, other._pImpl);
	swap(_async, other._async);
	swap(_pAsyncExec, other._pAsyncExec);
	swap(_pResult, other._pResult);
	_arguments.swap(other._arguments);
	swap(_pRowFormatter, other._pRowFormatter);
}


Statement& Statement::reset(Session& session)
{
	Statement stmt(session.createStatementImpl());
	swap(stmt);
	return *this;
}


Statement::ResultType Statement::execute()
{
	Mutex::ScopedLock lock(_mutex);
	bool isDone = done();
	if (initialized() || paused() || isDone)
	{
		if (_arguments.size()) 
		{
			_pImpl->formatSQL(_arguments);
			_arguments.clear();
		}

		if (!isAsync())
		{
			if (isDone) _pImpl->reset();
			return _pImpl->execute();
		}
		else
		{
			doAsyncExec();
			return 0;
		}
	} else
		throw InvalidAccessException("Statement still executing.");
}


const Statement::Result& Statement::executeAsync()
{
	Mutex::ScopedLock lock(_mutex);
	if (initialized() || paused() || done())
		return doAsyncExec();
	else
		throw InvalidAccessException("Statement still executing.");
}


const Statement::Result& Statement::doAsyncExec()
{
	if (done()) _pImpl->reset();
	if (!_pAsyncExec)
		_pAsyncExec = new AsyncExecMethod(_pImpl, &StatementImpl::execute);
	_pResult = new Result((*_pAsyncExec)());
	return *_pResult;
}


void Statement::setAsync(bool async)
{
	_async = async;
	if (_async && !_pAsyncExec)
		_pAsyncExec = new AsyncExecMethod(_pImpl, &StatementImpl::execute);
}


Statement::ResultType Statement::wait(long milliseconds)
{
	if (!_pResult) return 0;

	if (WAIT_FOREVER != milliseconds)
		_pResult->wait(milliseconds);
	else
		_pResult->wait();

	if (_pResult->exception())
		throw *_pResult->exception();

	return _pResult->data();
}


const std::string& Statement::getStorage() const
{
	switch (storage())
	{
	case STORAGE_VECTOR:
		return StatementImpl::VECTOR;
	case STORAGE_LIST:
		return StatementImpl::LIST;
	case STORAGE_DEQUE:
		return StatementImpl::DEQUE;
	case STORAGE_UNKNOWN:
		return StatementImpl::UNKNOWN;
	}

	throw IllegalStateException("Invalid storage setting.");
}


Statement& Statement::operator , (Manipulator manip)
{
	manip(*this);
	return *this;
}


Statement& Statement::addBind(AbstractBinding* pBind, bool duplicate)
{
	if (pBind->isBulk())
	{
		if (!_pImpl->isBulkSupported())
			throw InvalidAccessException("Bulk not supported by this session.");

		if(_pImpl->bulkBindingAllowed())
			_pImpl->setBulkBinding();
		else
			throw InvalidAccessException("Bulk and non-bulk binding modes can not be mixed.");
	}
	else _pImpl->forbidBulk();

	if (duplicate) pBind->duplicate();
	_pImpl->addBind(pBind);
	return *this;
}


Statement& Statement::addExtract(AbstractExtraction* pExtract, bool duplicate)
{
	if (pExtract->isBulk())
	{
		if (!_pImpl->isBulkSupported())
			throw InvalidAccessException("Bulk not supported by this session.");

		if(_pImpl->bulkExtractionAllowed())
		{
			Bulk b(pExtract->getLimit());
			_pImpl->setBulkExtraction(b);
		}
		else
			throw InvalidAccessException("Bulk and non-bulk extraction modes can not be mixed.");
	}
	else _pImpl->forbidBulk();

	if (duplicate) pExtract->duplicate();
	_pImpl->addExtract(pExtract);
	return *this;
}


Statement& Statement::operator , (const Limit& extrLimit)
{
	if (_pImpl->isBulkExtraction() && _pImpl->extractionLimit() != extrLimit)
		throw InvalidArgumentException("Limit for bulk extraction already set.");

	_pImpl->setExtractionLimit(extrLimit);
	return *this;
}


Statement& Statement::operator , (const Range& extrRange)
{
	if (_pImpl->isBulkExtraction())
		throw InvalidAccessException("Can not set range for bulk extraction.");

	_pImpl->setExtractionLimit(extrRange.lower());
	_pImpl->setExtractionLimit(extrRange.upper());
	return *this;
}


Statement& Statement::operator , (const Bulk& bulk)
{
	if (!_pImpl->isBulkSupported())
			throw InvalidAccessException("Bulk not supported by this session.");

	if (0 == _pImpl->extractions().size() && 
		0 == _pImpl->bindings().size() &&
		_pImpl->bulkExtractionAllowed() &&
		_pImpl->bulkBindingAllowed())
	{
		_pImpl->setBulkExtraction(bulk);
		_pImpl->setBulkBinding();
	}
	else
		throw InvalidAccessException("Can not set bulk operations.");

	return *this;
}


Statement& Statement::operator , (BulkFnType)
{
	const Limit& limit(_pImpl->extractionLimit());
	if (limit.isHardLimit() || 
		limit.isLowerLimit() || 
		Limit::LIMIT_UNLIMITED == limit.value())
	{
		throw InvalidAccessException("Bulk is only allowed with limited extraction,"
			"non-hard and zero-based limits.");
	}

	Bulk bulk(limit);
	_pImpl->setBulkExtraction(bulk);
	_pImpl->setBulkBinding();

	return *this;
}


} } // namespace Poco::Data
