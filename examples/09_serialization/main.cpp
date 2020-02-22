#include <QCoreApplication>
#include <QDebug>
#include <QFile>
#include <QFileInfo>

#include <QUaServer>

#include "temperaturesensor.h"
#include "quaxmlserializer.h"

void setupDefaultAddressSpace(QUaServer &server)
{
	QUaFolderObject* objsFolder = server.objectsFolder();

	// create default

	QUaBaseDataVariable* varBaseData = objsFolder->addBaseDataVariable();
	varBaseData->setWriteAccess(true);
	varBaseData->setBrowseName("my_variable");
	varBaseData->setDisplayName("my_variable");
	varBaseData->setValue(1);

	QUaProperty* varProp = objsFolder->addProperty("ns=1;s=my_prop");
	varProp->setBrowseName("my_property");
	varProp->setDisplayName("my_property");
	varProp->setValue("hola");

	QUaBaseObject* objBase = objsFolder->addBaseObject("ns=1;s=my_obj");
	objBase->setBrowseName("my_object");
	objBase->setDisplayName("my_object");

	QUaFolderObject* objFolder = objsFolder->addFolderObject();
	objFolder->setBrowseName("my_folder");
	objFolder->setDisplayName("my_folder");

	QUaProperty* varSubProp = objBase->addProperty();
	varSubProp->setBrowseName("my_sub_property");
	varSubProp->setDisplayName("my_sub_property");
	varSubProp->setValue(666.7);

	QUaBaseObject* objSubBase = objBase->addBaseObject();
	objSubBase->setBrowseName("my_sub_object");
	objSubBase->setDisplayName("my_sub_object");

	QUaFolderObject* objSubFolder = objBase->addFolderObject();
	objSubFolder->setBrowseName("my_sub_folder");
	objSubFolder->setDisplayName("my_sub_folder");

	QUaBaseObject* objSubSubBase = objSubFolder->addBaseObject("ns=1;s=my_subsub_object");
	objSubSubBase->setBrowseName("my_subsub_object");
	objSubSubBase->setDisplayName("my_subsub_object");

	// instances of custom type
	auto sensor1 = objSubSubBase->addChild<TemperatureSensor>();
	sensor1->setDisplayName("Sensor1");
	sensor1->setBrowseName("Sensor1");
	auto sensor2 = objSubSubBase->addChild<TemperatureSensor>();
	sensor2->setDisplayName("Sensor2");
	sensor2->setBrowseName("Sensor2");
	auto sensor3 = objSubSubBase->addChild<TemperatureSensor>();
	sensor3->setDisplayName("Sensor3");
	sensor3->setBrowseName("Sensor3");
}

int main(int argc, char *argv[])
{
	QCoreApplication a(argc, argv);

	const QString strFileName = "config.xml";

	QUaServer server;
	server.registerType<TemperatureSensor>();

	QUaXmlSerializer serializer;

	QUaFolderObject* objsFolder = server.objectsFolder();

	QFileInfo fileInfoConf(strFileName);
	if (fileInfoConf.exists())
	{
		// load from file
		QFile fileConf(strFileName);
		if (!fileConf.open(QIODevice::ReadOnly | QIODevice::Text))
		{
			qCritical() << "Could not open config file for read" << strFileName;
			return 1;
		}
		// load all data
		QQueue<QUaLog> logOut;
		if (!serializer.fromByteArray(&server, fileConf.readAll(), logOut))
		{
			// print log entries if any
			for (auto log : logOut)
			{
				qWarning() << "[" << log.level << "] :" << log.message;
			}
			// close file
			fileConf.close();
			// exit
			return 1;
		}
		// deserialize
		QUaFolderObject* objsFolder = server.objectsFolder();
		bool ok = objsFolder->deserialize(serializer, logOut);
		Q_ASSERT(ok);
		// print log entries if any
		for (auto log : logOut)
		{
			qWarning() << "[" << log.level << "] :" << log.message;
		}
	}
	else
	{
		// create some objects and variables to test
		setupDefaultAddressSpace(server);
		QUaFolderObject* objsFolder = server.objectsFolder();
		// serialize
		QQueue<QUaLog> logOut;
		bool ok = objsFolder->serialize(serializer, logOut);
		Q_ASSERT(ok);
		// save to file
		QFile fileConf(strFileName);
		if (fileConf.open(QIODevice::WriteOnly | QIODevice::Text | QFile::Truncate))
		{
			// create stream
			QTextStream streamConfig(&fileConf);
			// save config in file
			auto b = serializer.toByteArray();
			streamConfig << b;
		}
		// close file
		fileConf.close();
	}

	// logging
	QObject::connect(&server, &QUaServer::logMessage,
	[](const QUaLog& log) {
		qDebug() << "[" << log.level << "] :" << log.message;
	});

	server.start();

	return a.exec(); 
}