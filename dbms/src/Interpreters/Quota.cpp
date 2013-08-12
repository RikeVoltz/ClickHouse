#include <DB/Interpreters/Quota.h>


namespace DB
{

void QuotaValues::initFromConfig(const String & config_elem)
{
	Poco::Util::AbstractConfiguration & config = Poco::Util::Application::instance().config();

	queries 	= config.getInt(config_elem + ".queries", 		0);
	errors 		= config.getInt(config_elem + ".errors", 		0);
	result_rows = config.getInt(config_elem + ".result_rows",	0);
	read_rows 	= config.getInt(config_elem + ".read_rows", 	0);
	execution_time = Poco::Timespan(config.getInt(config_elem + ".execution_time", 0), 0);
}


void QuotaForInterval::initFromConfig(const String & config_elem, time_t duration_)
{
	rounded_time = 0;
	duration = duration_;
	max.initFromConfig(config_elem);
}

void QuotaForInterval::checkExceeded(time_t current_time, const String & quota_name)
{
	updateTime(current_time);
	check(max.queries, used.queries, current_time, quota_name, "Queries");
	check(max.errors, used.errors, current_time, quota_name, "Errors");
	check(max.result_rows, used.result_rows, current_time, quota_name, "Total result rows");
	check(max.read_rows, used.read_rows, current_time, quota_name, "Total rows read");
	check(max.execution_time.totalSeconds(), used.execution_time.totalSeconds(), current_time, quota_name, "Total execution time");

	std::cerr << "Current values for interval " << mysqlxx::DateTime(rounded_time) << " - " << mysqlxx::DateTime(rounded_time + duration) << ":\n"
		<< "queries: " << used.queries << "\n"
		<< "errors: " << used.errors << "\n";
}

void QuotaForInterval::addQuery(time_t current_time, const String & quota_name)
{
	++used.queries;
}

void QuotaForInterval::addError(time_t current_time, const String & quota_name)
{
	++used.errors;
}

void QuotaForInterval::checkAndAddResultRows(time_t current_time, const String & quota_name, size_t ammount)
{
	checkExceeded(current_time, quota_name);
	used.result_rows += ammount;
}

void QuotaForInterval::checkAndAddReadRows(time_t current_time, const String & quota_name, size_t ammount)
{
	checkExceeded(current_time, quota_name);
	used.read_rows += ammount;
}

void QuotaForInterval::checkAndAddExecutionTime(time_t current_time, const String & quota_name, Poco::Timespan ammount)
{
	checkExceeded(current_time, quota_name);
	used.execution_time += ammount;
}

void QuotaForInterval::updateTime(time_t current_time)
{
	if (current_time >= rounded_time + static_cast<int>(duration))
	{
		rounded_time = current_time / duration * duration;
		used.clear();
	}
}

void QuotaForInterval::check(size_t max_ammount, size_t used_ammount, time_t current_time, const String & quota_name, const char * resource_name)
{
	if (max_ammount && used_ammount >= max_ammount)
	{
		std::stringstream message;
		message << "Quota '" << quota_name << "' for ";

		if (duration == 3600)
			message << "1 hour";
		else if (duration == 60)
			message << "1 minute";
		else if (duration % 3600 == 0)
			message << (duration / 3600) << " hours";
		else if (duration % 60 == 0)
			message << (duration / 60) << " minutes";
		else
			message << duration << " seconds";

		message << " has been expired. "
			<< resource_name << ": " << used_ammount << ", max: " << max_ammount << ". "
			<< "Interval will end at " << mysqlxx::DateTime(rounded_time + duration) << ".";

		throw Exception(message.str(), ErrorCodes::QUOTA_EXPIRED);
	}
}


void QuotaForIntervals::initFromConfig(const String & config_elem)
{
	Poco::Util::AbstractConfiguration & config = Poco::Util::Application::instance().config();

	Poco::Util::AbstractConfiguration::Keys config_keys;
	config.keys(config_elem, config_keys);

	for (Poco::Util::AbstractConfiguration::Keys::const_iterator it = config_keys.begin(); it != config_keys.end(); ++it)
	{
		if (0 != it->compare(0, strlen("interval"), "interval"))
			continue;

		String interval_config_elem = config_elem + "." + *it;
		time_t duration = config.getInt(interval_config_elem + ".duration");

		cont[duration].initFromConfig(interval_config_elem, duration);
	}
}

void QuotaForIntervals::checkExceeded(time_t current_time)
{
	Poco::ScopedLock<Poco::FastMutex> lock(parent->mutex);
	for (Container::reverse_iterator it = cont.rbegin(); it != cont.rend(); ++it)
		it->second.checkExceeded(current_time, parent->name);
}

void QuotaForIntervals::addQuery(time_t current_time)
{
	Poco::ScopedLock<Poco::FastMutex> lock(parent->mutex);
	for (Container::reverse_iterator it = cont.rbegin(); it != cont.rend(); ++it)
		it->second.addQuery(current_time, parent->name);
}

void QuotaForIntervals::addError(time_t current_time)
{
	Poco::ScopedLock<Poco::FastMutex> lock(parent->mutex);
	for (Container::reverse_iterator it = cont.rbegin(); it != cont.rend(); ++it)
		it->second.addError(current_time, parent->name);
}

void QuotaForIntervals::checkAndAddResultRows(time_t current_time, size_t ammount)
{
	Poco::ScopedLock<Poco::FastMutex> lock(parent->mutex);
	for (Container::reverse_iterator it = cont.rbegin(); it != cont.rend(); ++it)
		it->second.checkAndAddResultRows(current_time, parent->name, ammount);
}

void QuotaForIntervals::checkAndAddReadRows(time_t current_time, size_t ammount)
{
	Poco::ScopedLock<Poco::FastMutex> lock(parent->mutex);
	for (Container::reverse_iterator it = cont.rbegin(); it != cont.rend(); ++it)
		it->second.checkAndAddReadRows(current_time, parent->name, ammount);
}

void QuotaForIntervals::checkAndAddExecutionTime(time_t current_time, Poco::Timespan ammount)
{
	Poco::ScopedLock<Poco::FastMutex> lock(parent->mutex);
	for (Container::reverse_iterator it = cont.rbegin(); it != cont.rend(); ++it)
		it->second.checkAndAddExecutionTime(current_time, parent->name, ammount);
}


void Quota::initFromConfig(const String & config_elem, const String & name_)
{
	name = name_;

	Poco::Util::AbstractConfiguration & config = Poco::Util::Application::instance().config();

	keyed_by_ip = config.has(config_elem + ".keyed_by_ip");
	is_keyed = keyed_by_ip || config.has(config_elem + ".keyed");

	max.initFromConfig(config_elem);
}

QuotaForIntervals & Quota::get(const String & quota_key, const Poco::Net::IPAddress & ip)
{
	if (!quota_key.empty() && (!is_keyed || keyed_by_ip))
		throw Exception("Quota " + name + " doesn't allow client supplied keys.", ErrorCodes::QUOTA_DOESNT_ALLOW_KEYS);

	String quota_key_or_ip = keyed_by_ip ? ip.toString() : quota_key;
	UInt64 quota_key_hashed = 0;

	if (!quota_key_or_ip.empty())
	{
		union
		{
			char bytes[16];
			UInt64 u64[2];
		};

		SipHash hash;
		hash.update(quota_key_or_ip.data(), quota_key_or_ip.size());
		hash.final(bytes);

		quota_key_hashed = u64[0] ^ u64[1];
	}

	Poco::ScopedLock<Poco::FastMutex> lock(mutex);

	Container::iterator it = quota_for_keys.find(quota_key_hashed);
	if (quota_for_keys.end() == it)
	{
		it = quota_for_keys.insert(std::make_pair(quota_key_hashed, QuotaForIntervals(this))).first;
		it->second = max;
	}

	return it->second;
}


void Quotas::initFromConfig()
{
	Poco::Util::AbstractConfiguration & config = Poco::Util::Application::instance().config();

	Poco::Util::AbstractConfiguration::Keys config_keys;
	config.keys("quotas", config_keys);

	for (Poco::Util::AbstractConfiguration::Keys::const_iterator it = config_keys.begin(); it != config_keys.end(); ++it)
	{
		cont[*it] = new Quota();
		cont[*it]->initFromConfig("quotas." + *it, *it);
	}
}

QuotaForIntervals & Quotas::get(const String & name, const String & quota_key, const Poco::Net::IPAddress & ip)
{
	Container::iterator it = cont.find(name);
	if (cont.end() == it)
		throw Exception("Unknown quota " + name, ErrorCodes::UNKNOWN_QUOTA);

	return it->second->get(quota_key, ip);
}

}
