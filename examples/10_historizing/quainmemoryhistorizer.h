#ifndef QUAINMEMORYHISTORIZER_H
#define QUAINMEMORYHISTORIZER_H

#include <QUaHistoryBackend>

#ifdef UA_ENABLE_HISTORIZING

class QUaInMemoryHistorizer
{
public:

	// required API for QUaServer::setHistorizer
	// write data point to backend, return true on success
	bool writeHistoryData(
		const QString &strNodeId,
		const QUaHistoryDataPoint& dataPoint,
		QQueue<QUaLog>& logOut
	);
	// required API for QUaServer::setHistorizer
	// update an existing node's data point in backend, return true on success
	bool updateHistoryData(
		const QString& strNodeId,
		const QUaHistoryDataPoint& dataPoint,
		QQueue<QUaLog>& logOut
	);
	// required API for QUaServer::setHistorizer
	// remove an existing node's data points within a range, return true on success
	bool removeHistoryData(
		const QString& strNodeId,
		const QDateTime& timeStart,
		const QDateTime& timeEnd,
		QQueue<QUaLog>& logOut
	);
	// required API for QUaServer::setHistorizer
	// return the timestamp of the first sample available for the given node
	QDateTime firstTimestamp(
		const QString& strNodeId,
		QQueue<QUaLog>& logOut
	) const;
	// required API for QUaServer::setHistorizer
	// return the timestamp of the latest sample available for the given node
	QDateTime lastTimestamp(
		const QString& strNodeId,
		QQueue<QUaLog>& logOut
	) const;
	// required API for QUaServer::setHistorizer
	// return true if given timestamp is available for the given node
	bool hasTimestamp(
		const QString& strNodeId,
		const QDateTime& timestamp,
		QQueue<QUaLog>& logOut
	) const;
	// required API for QUaServer::setHistorizer
	// return a timestamp matching the criteria for the given node
	QDateTime findTimestamp(
		const QString& strNodeId,
		const QDateTime& timestamp,
		const QUaHistoryBackend::TimeMatch& match,
		QQueue<QUaLog>& logOut
	) const;
	// required API for QUaServer::setHistorizer
	// return the number for data points within a time range for the given node
	quint64 numDataPointsInRange(
		const QString& strNodeId,
		const QDateTime& timeStart,
		const QDateTime& timeEnd,
		QQueue<QUaLog>& logOut
	) const;
	// required API for QUaServer::setHistorizer
	// return the numPointsToRead data points for the given node from the given start time
	QVector<QUaHistoryDataPoint> readHistoryData(
		const QString   &strNodeId,
		const QDateTime &timeStart,
		const quint64   &numPointsToRead,
		QQueue<QUaLog>  &logOut
	) const;

private:
	struct DataPoint
	{
		QVariant  value;
		quint32   status;
	};
	// NOTE : use a map to store the data points of a single node, ordered by time
	typedef QMap<QDateTime, DataPoint> DataPointTable;
	QHash<QString, DataPointTable> m_database;
};

#endif // UA_ENABLE_HISTORIZING

#endif // QUAINMEMORYHISTORIZER_H
