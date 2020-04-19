#include "quanode.h"

#include <QUaServer>
#include <QUaProperty>
#include <QUaBaseDataVariable>
#include <QUaFolderObject>

#ifdef UA_ENABLE_SUBSCRIPTIONS_EVENTS
#include <QUaBaseEvent>
#endif // UA_ENABLE_SUBSCRIPTIONS_EVENTS
#ifdef UA_ENABLE_SUBSCRIPTIONS_ALARMS_CONDITIONS
#include <QUaCondition>
#endif // UA_ENABLE_SUBSCRIPTIONS_ALARMS_CONDITIONS

#include "quaserver_anex.h"

struct QUaInMemorySerializer
{
	bool QUaInMemorySerializer::writeInstance(
		const QString& nodeId,
		const QString& typeName,
		const QMap<QString, QVariant>& attrs,
		const QList<QUaForwardReference>& forwardRefs,
		QQueue<QUaLog>& logOut)
	{
		Q_ASSERT(!m_hashNodeTreeData.contains(nodeId));
		if (m_hashNodeTreeData.contains(nodeId))
		{
			logOut << QUaLog({
				QObject::tr("Error serializing node. Repeated NodeId %1").arg(nodeId),
				QUaLogLevel::Error,
				QUaLogCategory::Application
			});
			// the show must go on
			return true;
		}
		m_hashNodeTreeData[nodeId] = {
			typeName, attrs, forwardRefs
		};
		return true;
	};
	bool QUaInMemorySerializer::readInstance(
		const QString& nodeId,
		QString& typeName,
		QMap<QString, QVariant>& attrs,
		QList<QUaForwardReference>& forwardRefs,
		QQueue<QUaLog>& logOut)
	{
		Q_ASSERT(m_hashNodeTreeData.contains(nodeId));
		if (!m_hashNodeTreeData.contains(nodeId))
		{
			logOut << QUaLog({
				QObject::tr("Error deserializing node. Could not find NodeId %1").arg(nodeId),
				QUaLogLevel::Error,
				QUaLogCategory::Application
			});
			// the show must go on
			return true;
		}
		auto& data  = m_hashNodeTreeData[nodeId];
		typeName    = data.typeName;
		attrs       = data.attrs;
		forwardRefs = data.forwardRefs;
		return true;
	};
	void QUaInMemorySerializer::clear()
	{
		m_hashNodeTreeData.clear();
	};
	void QUaInMemorySerializer::qtDebug(QUaServer * server, const QString &header)
	{
		qDebug() << header;
		auto listNodeIds = m_hashNodeTreeData.keys();
		std::sort(listNodeIds.begin(), listNodeIds.end(),
		[this, server](const QUaNodeId& nodeId1, const QUaNodeId& nodeId2) -> bool {
			QString browse1 = QUaQualifiedName::reduce(server->nodeById(nodeId1)->nodeBrowsePath());
			QString browse2 = QUaQualifiedName::reduce(server->nodeById(nodeId2)->nodeBrowsePath());
			return browse1 < browse2;
		});
		for (auto nodeId : listNodeIds)
		{
			QUaNode* node = server->nodeById(nodeId);
			QString browse = QUaQualifiedName::reduce(node->nodeBrowsePath());
			qDebug() << QString("%1 [%2] (%3)")
				.arg(browse)
				.arg(nodeId)
				.arg(m_hashNodeTreeData[nodeId].typeName);
			if (m_hashNodeTreeData[nodeId].attrs.contains("value"))
			{
				qDebug()
					<< m_hashNodeTreeData[nodeId].attrs["dataType"]
					<< m_hashNodeTreeData[nodeId].attrs["value"];
			}
		}
	}
	struct QUaNodeData
	{
		QString typeName;
		QMap<QString, QVariant> attrs;
		QList<QUaForwardReference> forwardRefs;
	};
	QHash<QUaNodeId, QUaNodeData> m_hashNodeTreeData;
};

QUaNode::QUaNode(
	QUaServer* server
)
{
	// [NOTE] : constructor of any QUaNode-derived class is not meant to be called by the user
	//          the constructor is called automagically by this library, and m_newNodeNodeId and
	//          m_newNodeMetaObject must be set in QUaServer before calling the constructor, as
	//          is used in QUaServer::uaConstructor
	Q_CHECK_PTR(server);
	Q_CHECK_PTR(server->m_newNodeNodeId);
	Q_CHECK_PTR(server->m_newNodeMetaObject);
	// NOTE : thes ecan be nullptr because during deserialization we bind children manually
	//Q_CHECK_PTR(server->m_newNodeMandatoryChildren);
	//Q_CHECK_PTR(server->m_newNodeOptionalChildren);
	const UA_NodeId   &nodeId     = *server->m_newNodeNodeId;
	const QMetaObject &metaObject = *server->m_newNodeMetaObject;
	QString strClassName = QString(metaObject.className());
	// check
	Q_ASSERT(server && !UA_NodeId_isNull(&nodeId));
	// bind itself, only good for constructors of derived classes, because UA constructor overwrites it
	// so we need to also set the context again in QUaServer::uaConstructor
	// set server instance
	this->m_qUaServer = server;
	// set c++ instance as context
	UA_Server_setNodeContext(server->m_server, nodeId, (void*)this);
	// set node id to c++ instance
	this->m_nodeId = nodeId;
	// ignore objects folder
	UA_NodeId objectsFolderNodeId = UA_NODEID_NUMERIC(0, UA_NS0ID_OBJECTSFOLDER);
	if (UA_NodeId_equal(&nodeId, &objectsFolderNodeId))
	{ 
		return; 
	}
	// get all UA children in advance, because if none, then better early exit
	auto chidrenNodeIds = QUaNode::getChildrenNodeIds(nodeId, server);
	if (chidrenNodeIds.count() <= 0)
	{ 
		return; 
	}
	// create hash of nodeId's by browse name, which must match Qt's metaprops
	QHash<QUaQualifiedName, UA_NodeId> mapChildren;
	for (int i = 0; i < chidrenNodeIds.count(); i++)
	{
		auto childNodeId = chidrenNodeIds[i];
		// read browse name
		QUaQualifiedName browseName = QUaNode::getBrowseName(childNodeId, server);
		Q_ASSERT(!mapChildren.contains( browseName));
		mapChildren[browseName] = childNodeId;
	}
	// list meta props
	int propCount  = metaObject.propertyCount();
	int propOffset = QUaNode::getPropsOffsetHelper(metaObject);
	int numProps   = 0;
	for (int i = propOffset; i < propCount; i++)
	{
		QMetaProperty metaProperty = metaObject.property(i);
		// check if not enum
		if (!metaProperty.isEnumType())
		{
			// check if available in meta-system
			if (!QMetaType::metaObjectForType(metaProperty.userType()))
			{ continue; }
			// check if OPC UA relevant type
			const QMetaObject propMetaObject = *QMetaType::metaObjectForType(metaProperty.userType());
			if (!propMetaObject.inherits(&QUaNode::staticMetaObject))
			{ continue; }
			// check if prop inherits from parent
			Q_ASSERT_X(!propMetaObject.inherits(&metaObject), "QUaNode Constructor", 
				"Qt MetaProperty type cannot inherit from Class.");
			if (propMetaObject.inherits(&metaObject))
			{ continue; }
		}
		// inc number of valid props
		numProps++;
		// the Qt meta property name must match the UA browse name
		QUaQualifiedName browseName = QString(metaProperty.name());
		Q_ASSERT(mapChildren.contains(browseName));
		// get child nodeId for child
		auto childNodeId = mapChildren.take(browseName);
		// get node context (C++ instance)
		auto nodeInstance = QUaNode::getNodeContext(childNodeId, server);
		Q_CHECK_PTR(nodeInstance);
		// assign C++ parent
		nodeInstance->setParent(this);
		nodeInstance->setObjectName(browseName);
		// [NOTE] writing a pointer value to a Q_PROPERTY did not work, 
		//        eventhough there appear to be some success cases on the internet
		//        so in the end we have to query children by object name
	} // for each prop
	// handle mandatory children of instance declarations
	Q_ASSERT(m_qUaServer->m_hashMandatoryChildren.contains(strClassName));
	const auto &mandatoryList = m_qUaServer->m_hashMandatoryChildren[strClassName];
	for (const auto browseName : mandatoryList)
	{
		Q_ASSERT(mapChildren.contains(browseName));
		// get child nodeId for child
		auto childNodeId = mapChildren.take(browseName);
		// get node context (C++ instance)
		auto nodeInstance = QUaNode::getNodeContext(childNodeId, server);
		Q_CHECK_PTR(nodeInstance);
		// assign C++ parent
		nodeInstance->setParent(this);
		nodeInstance->setObjectName(browseName);
	}
	// if assert below fails, review filter in QUaNode::getChildrenNodeIds
	Q_ASSERT_X(mapChildren.count() == 0, "QUaNode::QUaNode", "Children not bound properly.");
	// cleanup
	for (int i = 0; i < chidrenNodeIds.count(); i++)
	{
		UA_NodeId_clear(&chidrenNodeIds[i]);
	}
}

QUaNode::~QUaNode()
{
	// check if node id has been already removed from node store
	// i.e. child of deleted parent node, or ...
	UA_NodeId outNodeId;
	auto st = UA_Server_readNodeId(m_qUaServer->m_server, m_nodeId, &outNodeId);
	if (st == UA_STATUSCODE_BADNODEIDUNKNOWN)
	{
		// cleanup
		UA_NodeId_clear(&outNodeId);
		return;
	}
	Q_ASSERT(UA_NodeId_equal(&m_nodeId, &outNodeId));
	// cleanup
	UA_NodeId_clear(&outNodeId);
	// remove context, so we avoid double deleting in ua destructor when called
	st = UA_Server_setNodeContext(m_qUaServer->m_server, m_nodeId, nullptr);
	Q_ASSERT(st == UA_STATUSCODE_GOOD);
	// delete node in ua (NOTE : also delete references)
	st = UA_Server_deleteNode(m_qUaServer->m_server, m_nodeId, true);
	Q_ASSERT(st == UA_STATUSCODE_GOOD);
	Q_UNUSED(st);
	// trigger reference deleted, model change event, so client (UaExpert) auto refreshes tree
#ifdef UA_ENABLE_SUBSCRIPTIONS_EVENTS
	Q_CHECK_PTR(m_qUaServer->m_changeEvent);
	// add reference deleted change to buffer
	QUaNode* parent = qobject_cast<QUaNode*>(this->parent());
	m_qUaServer->addChange({
		parent ? parent->nodeId() : QUaTypesConverter::nodeIdToQString(UA_NODEID_NUMERIC(0, UA_NS0ID_SERVER)),
		parent ? parent->typeDefinitionNodeId() : QUaTypesConverter::nodeIdToQString(UA_NODEID_NUMERIC(0, UA_NS0ID_SERVERTYPE)),
		QUaChangeVerb::ReferenceDeleted // UaExpert does not recognize QUaChangeVerb::NodeAdded
	});	
#endif // UA_ENABLE_SUBSCRIPTIONS_EVENTS
}

bool QUaNode::operator==(const QUaNode & other) const
{
	return UA_NodeId_equal(&this->m_nodeId, &other.m_nodeId);
}

QUaServer * QUaNode::server() const
{
	return m_qUaServer;
}

QString QUaNode::displayName() const
{
	Q_CHECK_PTR(m_qUaServer);
	Q_ASSERT(!UA_NodeId_isNull(&m_nodeId));
	if (UA_NodeId_isNull(&m_nodeId))
	{
		return QString();
	}
	// read display name
	UA_LocalizedText outDisplayName;
	auto st = UA_Server_readDisplayName(m_qUaServer->m_server, m_nodeId, &outDisplayName);
	Q_ASSERT(st == UA_STATUSCODE_GOOD);
	Q_UNUSED(st);
	QString displayName = QUaTypesConverter::uaStringToQString(outDisplayName.text);
	// cleanup
	UA_LocalizedText_clear(&outDisplayName);
	// return
	return displayName;
}

void QUaNode::setDisplayName(const QString & displayName)
{
	Q_CHECK_PTR(m_qUaServer);
	Q_ASSERT(!UA_NodeId_isNull(&m_nodeId));
	// convert to UA_LocalizedText
	QByteArray byteDisplayName = displayName.toUtf8(); // NOTE : QByteArray must exist in stack
    UA_LocalizedText uaDisplayName = UA_LOCALIZEDTEXT((char*)"", byteDisplayName.data());
	// set value
	auto st = UA_Server_writeDisplayName(m_qUaServer->m_server, m_nodeId, uaDisplayName);
	Q_ASSERT(st == UA_STATUSCODE_GOOD);
	Q_UNUSED(st);
	// emit displayName changed
	emit this->displayNameChanged(displayName);
}

QString QUaNode::description() const
{
	Q_CHECK_PTR(m_qUaServer);
	Q_ASSERT(!UA_NodeId_isNull(&m_nodeId));
	if (UA_NodeId_isNull(&m_nodeId))
	{
		return QString();
	}
	// read description
	UA_LocalizedText outDescription;
	auto st = UA_Server_readDescription(m_qUaServer->m_server, m_nodeId, &outDescription);
	Q_ASSERT(st == UA_STATUSCODE_GOOD);
	Q_UNUSED(st);
	// return
	return QUaTypesConverter::uaStringToQString(outDescription.text);
}

void QUaNode::setDescription(const QString & description)
{
	Q_CHECK_PTR(m_qUaServer);
	Q_ASSERT(!UA_NodeId_isNull(&m_nodeId));
	// convert to UA_LocalizedText
	QByteArray byteDescription = description.toUtf8(); // NOTE : QByteArray must exist in stack
	UA_LocalizedText uaDescription = UA_LOCALIZEDTEXT((char*)"", byteDescription.data());
	// set value
	auto st = UA_Server_writeDescription(m_qUaServer->m_server, m_nodeId, uaDescription);
	Q_ASSERT(st == UA_STATUSCODE_GOOD);
	Q_UNUSED(st);
	// emit description changed
	emit this->descriptionChanged(description);
}

quint32 QUaNode::writeMask() const
{
	Q_CHECK_PTR(m_qUaServer);
	Q_ASSERT(!UA_NodeId_isNull(&m_nodeId));
	if (UA_NodeId_isNull(&m_nodeId))
	{
		return quint32();
	}
	// read writeMask
	UA_UInt32 outWriteMask;
	auto st = UA_Server_readWriteMask(m_qUaServer->m_server, m_nodeId, &outWriteMask);
	Q_ASSERT(st == UA_STATUSCODE_GOOD);
	Q_UNUSED(st);
	// return
	return outWriteMask;
}

void QUaNode::setWriteMask(const quint32 & writeMask)
{
	Q_CHECK_PTR(m_qUaServer);
	Q_ASSERT(!UA_NodeId_isNull(&m_nodeId));
	// set value
	auto st = UA_Server_writeWriteMask(m_qUaServer->m_server, m_nodeId, writeMask);
	Q_ASSERT(st == UA_STATUSCODE_GOOD);
	Q_UNUSED(st);
	// emit writeMask changed
	emit this->writeMaskChanged(writeMask);
}

QUaNodeId QUaNode::nodeId() const
{
	Q_CHECK_PTR(m_qUaServer);
	Q_ASSERT(!UA_NodeId_isNull(&m_nodeId));
	if (UA_NodeId_isNull(&m_nodeId))
	{
		return QUaNodeId();
	}
	return m_nodeId;
}

QString QUaNode::nodeClass() const
{
	Q_CHECK_PTR(m_qUaServer);
	Q_ASSERT(!UA_NodeId_isNull(&m_nodeId));
	if (UA_NodeId_isNull(&m_nodeId))
	{
		return QString();
	}
	// read nodeClass
	UA_NodeClass outNodeClass;
	auto st = UA_Server_readNodeClass(m_qUaServer->m_server, m_nodeId, &outNodeClass);
	Q_ASSERT(st == UA_STATUSCODE_GOOD);
	Q_UNUSED(st);
	// convert to QString
	return QUaTypesConverter::nodeClassToQString(outNodeClass);
}

QUaQualifiedName QUaNode::browseName() const
{
	Q_CHECK_PTR(m_qUaServer);
	Q_ASSERT(!UA_NodeId_isNull(&m_nodeId));
	if (UA_NodeId_isNull(&m_nodeId))
	{
		return QString();
	}
	// read browse name
	UA_QualifiedName outBrowseName;
	auto st = UA_Server_readBrowseName(m_qUaServer->m_server, m_nodeId, &outBrowseName);
	Q_ASSERT(st == UA_STATUSCODE_GOOD);
	Q_UNUSED(st);
	// populate return value
	QUaQualifiedName browseName = outBrowseName;
	// cleanup
	UA_QualifiedName_clear(&outBrowseName);
	return browseName;
}

QUaProperty * QUaNode::addProperty(
	const QUaQualifiedName& browseName,
	const QUaNodeId& nodeId/* = ""*/
)
{
	return m_qUaServer->createInstance<QUaProperty>(this, browseName, nodeId);
}

QUaBaseDataVariable* QUaNode::addBaseDataVariable(
	const QUaQualifiedName& browseName,
	const QUaNodeId& nodeId/* = ""*/
)
{
	return m_qUaServer->createInstance<QUaBaseDataVariable>(this, browseName, nodeId);
}

QUaBaseObject* QUaNode::addBaseObject(
	const QUaQualifiedName& browseName,
	const QUaNodeId& nodeId/* = ""*/
)
{
	return m_qUaServer->createInstance<QUaBaseObject>(this, browseName, nodeId);
}

QUaFolderObject* QUaNode::addFolderObject(
	const QUaQualifiedName& browseName,
	const QUaNodeId& nodeId/* = ""*/
)
{
	return m_qUaServer->createInstance<QUaFolderObject>(this, browseName, nodeId);
}

QUaNodeId QUaNode::typeDefinitionNodeId() const
{
	Q_CHECK_PTR(m_qUaServer);
	Q_ASSERT(!UA_NodeId_isNull(&m_nodeId));
	if (UA_NodeId_isNull(&m_nodeId))
	{
		return QUaNodeId();
	}
	UA_NodeId retTypeId = QUaNode::typeDefinitionNodeId(m_nodeId, m_qUaServer->m_server);
	// return in string form
	QUaNodeId retNodeId = retTypeId;
	UA_NodeId_clear(&retTypeId);
	return retNodeId;
}

UA_NodeId QUaNode::typeDefinitionNodeId(
	const UA_NodeId& nodeId, 
	UA_Server* server
)
{
	UA_NodeId retTypeId = UA_NODEID_NULL;
	// make ua browse
	UA_BrowseDescription* bDesc = UA_BrowseDescription_new();
	UA_NodeId_copy(&nodeId, &bDesc->nodeId); // from child
	bDesc->browseDirection = UA_BROWSEDIRECTION_FORWARD;
	bDesc->includeSubtypes = true;
	bDesc->resultMask = UA_BROWSERESULTMASK_REFERENCETYPEID;
	bDesc->referenceTypeId = UA_NODEID_NUMERIC(0, UA_NS0ID_HASTYPEDEFINITION);
	// browse
	UA_BrowseResult bRes = UA_Server_browse(server, 0, bDesc);
	Q_ASSERT(bRes.statusCode == UA_STATUSCODE_GOOD);
	Q_ASSERT(bRes.referencesSize == 1);
	if (bRes.referencesSize < 1)
	{
		return retTypeId;
	}
	UA_ReferenceDescription rDesc = bRes.references[0];
	UA_NodeId_copy(&rDesc.nodeId.nodeId, &retTypeId);
	// cleanup
	UA_BrowseDescription_deleteMembers(bDesc);
	UA_BrowseDescription_delete(bDesc);
	UA_BrowseResult_deleteMembers(&bRes);
	// return
	// NOTE : need to clean up returned value
	return retTypeId;
}

UA_NodeId QUaNode::superTypeDefinitionNodeId(
	const UA_NodeId& typeNodeId,
	UA_Server* server
)
{
	UA_NodeId retTypeId = UA_NODEID_NULL;
	// make ua browse
	UA_BrowseDescription* bDesc = UA_BrowseDescription_new();
	UA_NodeId_copy(&typeNodeId, &bDesc->nodeId); // from child
	bDesc->browseDirection = UA_BROWSEDIRECTION_INVERSE;
	bDesc->includeSubtypes = true;
	bDesc->resultMask = UA_BROWSERESULTMASK_REFERENCETYPEID;
	bDesc->referenceTypeId = UA_NODEID_NUMERIC(0, UA_NS0ID_HASSUBTYPE);
	// browse
	UA_BrowseResult bRes = UA_Server_browse(server, 0, bDesc);
	Q_ASSERT(bRes.statusCode == UA_STATUSCODE_GOOD);
	Q_ASSERT(bRes.referencesSize == 1);
	if (bRes.referencesSize < 1)
	{
		return retTypeId;
	}
	UA_ReferenceDescription rDesc = bRes.references[0];
	UA_NodeId_copy(&rDesc.nodeId.nodeId, &retTypeId);
	// cleanup
	UA_BrowseDescription_deleteMembers(bDesc);
	UA_BrowseDescription_delete(bDesc);
	UA_BrowseResult_deleteMembers(&bRes);
	// return
	// NOTE : need to clean up returned value
	return retTypeId;
}

QString QUaNode::typeDefinitionDisplayName() const
{
	Q_CHECK_PTR(m_qUaServer);
	Q_ASSERT(!UA_NodeId_isNull(&m_nodeId));
	if (UA_NodeId_isNull(&m_nodeId))
	{
		return QString();
	}
	UA_NodeId typeId = this->typeDefinitionNodeId();
	Q_ASSERT(!UA_NodeId_isNull(&typeId));
	if (UA_NodeId_isNull(&typeId))
	{
		return QString();
	}
	// read display name
	UA_LocalizedText outDisplayName;
	auto st = UA_Server_readDisplayName(m_qUaServer->m_server, typeId, &outDisplayName);
	Q_ASSERT(st == UA_STATUSCODE_GOOD);
	Q_UNUSED(st);
	QString displayName = QUaTypesConverter::uaStringToQString(outDisplayName.text);
	// cleanup
	UA_NodeId_clear(&typeId);
	UA_LocalizedText_clear(&outDisplayName);
	// return
	return displayName;
}

QUaQualifiedName QUaNode::typeDefinitionBrowseName() const
{
	Q_CHECK_PTR(m_qUaServer);
	Q_ASSERT(!UA_NodeId_isNull(&m_nodeId));
	if (UA_NodeId_isNull(&m_nodeId))
	{
		return QUaQualifiedName();
	}
	UA_NodeId typeId = this->typeDefinitionNodeId();
	Q_ASSERT(!UA_NodeId_isNull(&typeId));
	if (UA_NodeId_isNull(&typeId))
	{
		return QUaQualifiedName();
	}
	// read browse name
	UA_QualifiedName outBrowseName;
	auto st = UA_Server_readBrowseName(m_qUaServer->m_server, typeId, &outBrowseName);
	Q_ASSERT(st == UA_STATUSCODE_GOOD);
	Q_UNUSED(st);
	// populate return value
	QUaQualifiedName  browseName = outBrowseName;
	// cleanup
	UA_QualifiedName_clear(&outBrowseName);
	return  browseName;
}

QList<QUaNode*> QUaNode::browseChildren() const
{
	// TODO : check if faster with open62541 browse API
	return this->findChildren<QUaNode*>(QString(), Qt::FindDirectChildrenOnly);
}

QUaNode* QUaNode::browseChild(
	const QUaQualifiedName&  browseName,
	const bool& instantiateOptional/* = false*/)
{
	// first check cache
	QUaNode* child = m_browseCache.value(browseName, nullptr);
	if (child)
	{
		return child;
	}
	// TODO : check if new open62541 tree-based lookup implementation is faster
	child = this->findChild<QUaNode*>(browseName, Qt::FindDirectChildrenOnly);
	if (child)
	{
		m_browseCache[browseName] = child;
		QObject::connect(child, &QObject::destroyed, this, [this, browseName]() {
			m_browseCache.remove(browseName);
		});	
		return child;
	}
	if (!instantiateOptional)
	{
		return nullptr;
	}
	child = this->instantiateOptionalChild(browseName);
	Q_ASSERT_X(child, "QUaNode::browseChild", "TODO : error log");
	if (child)
	{
		m_browseCache[browseName] = child;
		QObject::connect(child, &QObject::destroyed, this, [this, browseName]() {
			m_browseCache.remove(browseName);
		});	
	}
	return child;
}

bool QUaNode::hasChild(const QUaQualifiedName &browseName)
{
	return this->browseChild(browseName);
}

QUaNode * QUaNode::browsePath(const QUaQualifiedNameList& browsePath) const
{
	QUaNode * currNode = const_cast<QUaNode *>(this);
	for (auto browseName : browsePath)
	{
		currNode = currNode->browseChild(browseName);
		if (!currNode)
		{
			return nullptr;
		}
	}
	return currNode;
}

QUaQualifiedNameList QUaNode::nodeBrowsePath() const
{
	// get parents browse path and then attach current browse name
	// stop recursion if current node is ObjectsFolder
	if (this == m_qUaServer->objectsFolder())
	{
		return QUaQualifiedNameList() << this->browseName();
	}
#ifdef QT_DEBUG 
	QUaNode* parent = qobject_cast<QUaNode*>(this->parent());
	// handle hidden nodes (i.e. branches)
#ifndef UA_ENABLE_SUBSCRIPTIONS_ALARMS_CONDITIONS
	Q_ASSERT(parent);
#else
	Q_ASSERT(parent || qobject_cast<const QUaCondition*>(this));
#endif // !UA_ENABLE_SUBSCRIPTIONS_ALARMS_CONDITIONS
	if (!parent)
	{
		return QUaQualifiedNameList() << m_qUaServer->objectsFolder()->browseName() << this->browseName();
	}
#else
	QUaNode* parent = static_cast<QUaNode*>(this->parent());
#endif // QT_DEBUG 
	return parent->nodeBrowsePath() << this->browseName();
}

void QUaNode::addReference(const QUaReferenceType& ref, QUaNode* nodeTarget, const bool& isForward/* = true*/)
{
	// first check if reference type is registered
	if (!m_qUaServer->m_hashRefTypes.contains(ref))
	{
		m_qUaServer->registerReferenceType(ref);
	}
	Q_ASSERT(m_qUaServer->m_hashRefTypes.contains(ref));
	// reject hierarchical references
	Q_ASSERT_X(!m_qUaServer->m_hashHierRefTypes.contains(ref), "QUaNode::addReference", "Cannot add hierarchical references using this method.");
	if (m_qUaServer->m_hashHierRefTypes.contains(ref))
	{
		return;
	}
	// check valid node target
	Q_ASSERT_X(nodeTarget, "QUaNode::addReference", "Invalid target node.");
	if (!nodeTarget)
	{
		return;
	}
	UA_NodeId refTypeId = m_qUaServer->m_hashRefTypes[ref];
	// check if reference already exists
	auto set = getRefsInternal(ref, isForward);
	if (set.contains(nodeTarget->m_nodeId))
	{
		// cleanup set
		QSetIterator<UA_NodeId> i(set);
		while (i.hasNext())
		{
			UA_NodeId nodeId = i.next();
			UA_NodeId_clear(&nodeId);
		}
		return;
	}
	// cleanup set
	QSetIterator<UA_NodeId> i(set);
	while (i.hasNext())
	{
		UA_NodeId nodeId = i.next();
		UA_NodeId_clear(&nodeId);
	}
	// add the reference
	auto st = UA_Server_addReference(
		m_qUaServer->m_server,
		m_nodeId,
		refTypeId,
		{ nodeTarget->m_nodeId, UA_STRING_NULL, 0 },
		isForward
	);
	Q_ASSERT(st == UA_STATUSCODE_GOOD);
	// emit events
	emit this->referenceAdded(ref, nodeTarget, isForward);
	emit nodeTarget->referenceAdded(ref, this, !isForward);
	// subscribe node destructions
	QObject::connect(nodeTarget, &QObject::destroyed, this,
		[this, ref, nodeTarget, isForward]() {
			// emit event
			emit this->referenceRemoved(ref, nodeTarget, isForward);
		});
	QObject::connect(this, &QObject::destroyed, nodeTarget,
		[this, ref, nodeTarget, isForward]() {
			// emit event
			emit nodeTarget->referenceRemoved(ref, this, !isForward);
		});
}

void QUaNode::removeReference(const QUaReferenceType& ref, QUaNode* nodeTarget, const bool& isForward/* = true*/)
{
	// first check if reference type is removeReference
	Q_ASSERT_X(m_qUaServer->m_hashRefTypes.contains(ref), "QUaNode::addReference", "Reference not registered.");
	if (!m_qUaServer->m_hashRefTypes.contains(ref))
	{
		return;
	}
	// check valid node target
	Q_ASSERT_X(nodeTarget, "QUaNode::removeReference", "Invalid target node.");
	if (!nodeTarget)
	{
		return;
	}
	UA_NodeId refTypeId = m_qUaServer->m_hashRefTypes[ref];
	// check reference exists
	auto set = getRefsInternal(ref, isForward);
	if (!set.contains(nodeTarget->m_nodeId))
	{
		// cleanup set
		QSetIterator<UA_NodeId> i(set);
		while (i.hasNext())
		{
			UA_NodeId nodeId = i.next();
			UA_NodeId_clear(&nodeId);
		}
		return;
	}
	// cleanup set
	QSetIterator<UA_NodeId> i(set);
	while (i.hasNext())
	{
		UA_NodeId nodeId = i.next();
		UA_NodeId_clear(&nodeId);
	}
	// remove the reference
	auto st = UA_Server_deleteReference(
		m_qUaServer->m_server,
		m_nodeId,
		refTypeId,
		isForward,
		{ nodeTarget->m_nodeId, UA_STRING_NULL, 0 },
		true
	);
	Q_ASSERT(st == UA_STATUSCODE_GOOD);
	// emit event
	emit this->referenceRemoved(ref, nodeTarget, isForward);
	emit nodeTarget->referenceRemoved(ref, this, !isForward);
	// unsubscribe node destructions
	QObject::disconnect(nodeTarget, &QObject::destroyed, this, 0);
	QObject::disconnect(this, &QObject::destroyed, nodeTarget, 0);
}

QList<QUaNode*> QUaNode::findReferences(const QUaReferenceType& ref, const bool& isForward /*= true*/) const
{
	QList<QUaNode*> retRefList;
	// call internal method
	auto set = getRefsInternal(ref, isForward);
	// get contexts
	QSetIterator<UA_NodeId> i(set);
	while (i.hasNext())
	{
		UA_NodeId nodeId = i.next();
		QUaNode* node = QUaNode::getNodeContext(nodeId, m_qUaServer);
		if (node)
		{
			Q_ASSERT(UA_NodeId_equal(&nodeId, &node->m_nodeId));
			retRefList.append(node);
		}
		// cleanup set
		UA_NodeId_clear(&nodeId);
	}
	// return
	return retRefList;
}

// NOTE : code borrowed from open62541.c addOptionalObjectField
UA_StatusCode QUaNode::addOptionalVariableField(
	UA_Server* server, 
	const UA_NodeId* originNode, 
	const UA_QualifiedName* fieldName, 
	const UA_VariableNode* optionalVariableFieldNode, 
	UA_NodeId* outOptionalVariable)
{
	UA_VariableAttributes vAttr = UA_VariableAttributes_default;
	vAttr.valueRank = optionalVariableFieldNode->valueRank;
	UA_StatusCode retval = UA_LocalizedText_copy(&optionalVariableFieldNode->displayName,
		&vAttr.displayName);
	CONDITION_ASSERT_RETURN_RETVAL(retval, "Copying LocalizedText failed", );

	retval = UA_NodeId_copy(&optionalVariableFieldNode->dataType, &vAttr.dataType);
	CONDITION_ASSERT_RETURN_RETVAL(retval, "Copying NodeId failed", );

	// missing to copy dimenstion size
	vAttr.arrayDimensionsSize = optionalVariableFieldNode->arrayDimensionsSize;
	vAttr.arrayDimensions = optionalVariableFieldNode->arrayDimensions;

	/* Get typedefintion */
	const UA_Node* type = getNodeType(server, (const UA_Node*)optionalVariableFieldNode);
	if (!type) {
		UA_LOG_WARNING(&server->config.logger, UA_LOGCATEGORY_USERLAND,
			"Invalid VariableType. StatusCode %s",
			UA_StatusCode_name(UA_STATUSCODE_BADTYPEDEFINITIONINVALID));
		UA_VariableAttributes_deleteMembers(&vAttr);
		return UA_STATUSCODE_BADTYPEDEFINITIONINVALID;
	}

	/* Set referenceType to parent */
	UA_NodeId referenceToParent;
	UA_NodeId propertyTypeNodeId = UA_NODEID_NUMERIC(0, UA_NS0ID_PROPERTYTYPE);
	if (UA_NodeId_equal(&type->nodeId, &propertyTypeNodeId))
		referenceToParent = UA_NODEID_NUMERIC(0, UA_NS0ID_HASPROPERTY);
	else
		referenceToParent = UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT);

	/* Set a random unused NodeId with specified Namespace Index*/
	UA_NodeId optionalVariable = { originNode->namespaceIndex, UA_NODEIDTYPE_NUMERIC, {0} };
	retval = UA_Server_addVariableNode(server, optionalVariable, *originNode,
		referenceToParent, *fieldName, type->nodeId,
		vAttr, NULL, outOptionalVariable);
	Q_ASSERT(retval == UA_STATUSCODE_GOOD);
	UA_NODESTORE_RELEASE(server, type);
	UA_VariableAttributes_deleteMembers(&vAttr);
	return retval;
}

// NOTE : code borrowed from open62541.c addOptionalObjectField
UA_StatusCode QUaNode::addOptionalObjectField(
	UA_Server* server, 
	const UA_NodeId* originNode,
	const UA_QualifiedName* fieldName, 
	const UA_ObjectNode* optionalObjectFieldNode, 
	UA_NodeId* outOptionalObject)
{
	UA_ObjectAttributes oAttr = UA_ObjectAttributes_default;
	UA_StatusCode retval = UA_LocalizedText_copy(&optionalObjectFieldNode->displayName,
		&oAttr.displayName);
	CONDITION_ASSERT_RETURN_RETVAL(retval, "Copying LocalizedText failed", );

	/* Get typedefintion */
	const UA_Node* type = getNodeType(server, (const UA_Node*)optionalObjectFieldNode);
	if (!type) {
		UA_LOG_WARNING(&server->config.logger, UA_LOGCATEGORY_USERLAND,
			"Invalid ObjectType. StatusCode %s",
			UA_StatusCode_name(UA_STATUSCODE_BADTYPEDEFINITIONINVALID));
		UA_ObjectAttributes_deleteMembers(&oAttr);
		return UA_STATUSCODE_BADTYPEDEFINITIONINVALID;
	}

	/* Set referenceType to parent */
	UA_NodeId referenceToParent;
	UA_NodeId propertyTypeNodeId = UA_NODEID_NUMERIC(0, UA_NS0ID_PROPERTYTYPE);
	if (UA_NodeId_equal(&type->nodeId, &propertyTypeNodeId))
		referenceToParent = UA_NODEID_NUMERIC(0, UA_NS0ID_HASPROPERTY);
	else
		referenceToParent = UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT);

	UA_NodeId optionalObject = { originNode->namespaceIndex, UA_NODEIDTYPE_NUMERIC, {0} };
	retval = UA_Server_addObjectNode(server, optionalObject, *originNode,
		referenceToParent, *fieldName, type->nodeId,
		oAttr, NULL, outOptionalObject);

	UA_NODESTORE_RELEASE(server, type);
	UA_ObjectAttributes_deleteMembers(&oAttr);
	return retval;
}

// NOTE : need to cleanup result after calling this method
QSet<UA_NodeId> QUaNode::getRefsInternal(const QUaReferenceType& ref, const bool & isForward /*= true*/) const
{
	QSet<UA_NodeId> retRefSet;
	// first check if reference type is registered
	if (!m_qUaServer->m_hashRefTypes.contains(ref))
	{
		m_qUaServer->registerReferenceType(ref);
		// there cannot be any since it didnt even exist before
		return retRefSet;
	}
	Q_ASSERT(m_qUaServer->m_hashRefTypes.contains(ref));
	UA_NodeId refTypeId = m_qUaServer->m_hashRefTypes[ref];
	// make ua browse
	UA_BrowseDescription * bDesc = UA_BrowseDescription_new();
	UA_NodeId_copy(&m_nodeId, &bDesc->nodeId); // from child
	bDesc->browseDirection = isForward ? UA_BROWSEDIRECTION_FORWARD : UA_BROWSEDIRECTION_INVERSE;
	bDesc->includeSubtypes = true;
	bDesc->nodeClassMask   = UA_NODECLASS_OBJECT | UA_NODECLASS_VARIABLE; // only objects or variables (no types or refs)
	bDesc->resultMask      = UA_BROWSERESULTMASK_REFERENCETYPEID;
	// browse
	UA_BrowseResult bRes = UA_Server_browse(m_qUaServer->m_server, 0, bDesc);
	Q_ASSERT(bRes.statusCode == UA_STATUSCODE_GOOD);
	while (bRes.referencesSize > 0)
	{
		for (size_t i = 0; i < bRes.referencesSize; i++)
		{
			UA_ReferenceDescription rDesc = bRes.references[i];
			if (UA_NodeId_equal(&rDesc.referenceTypeId, &refTypeId))
			{
				UA_NodeId nodeId/* = rDesc.nodeId.nodeId*/;
				UA_NodeId_copy(&rDesc.nodeId.nodeId, &nodeId);
				retRefSet.insert(nodeId);
			}
		}
		UA_BrowseResult_deleteMembers(&bRes);
		bRes = UA_Server_browseNext(m_qUaServer->m_server, true, &bRes.continuationPoint);
	}
	// cleanup
	UA_BrowseDescription_deleteMembers(bDesc);
	UA_BrowseDescription_delete(bDesc);
	UA_BrowseResult_deleteMembers(&bRes);
	// return
	return retRefSet;
}

QUaWriteMask QUaNode::userWriteMaskInternal(const QString & strUserName)
{
	// if has specific callback, use that one
	if (m_userWriteMaskCallback)
	{
		return m_userWriteMaskCallback(strUserName);
	}
	// else use possible reimplementation
	return this->userWriteMask(strUserName);
}

QUaAccessLevel QUaNode::userAccessLevelInternal(const QString & strUserName)
{
	// if has specific callback, use that one
	if (m_userAccessLevelCallback)
	{
		return m_userAccessLevelCallback(strUserName);
	}
	// else use possible reimplementation
	return this->userAccessLevel(strUserName);
}

bool QUaNode::userExecutableInternal(const QString & strUserName)
{
	// if has specific callback, use that one
	if (m_userExecutableCallback)
	{
		return m_userExecutableCallback(strUserName);
	}
	// else use possible reimplementation
	return this->userExecutable(strUserName);
}

// NOTE : some code borrowed from UA_Server_addConditionOptionalField
QUaNode* QUaNode::instantiateOptionalChild(const QUaQualifiedName&  browseName)
{
	// look if optional child really in model
	UA_NodeId optionalFieldNodeId = UA_NODEID_NULL;
	UA_NodeId typeNodeId = QUaNode::typeDefinitionNodeId(m_nodeId, m_qUaServer->m_server);
	// look for optional child starting from this type of to base object type
	while (!UA_NodeId_equal(&typeNodeId, &UA_NODEID_NUMERIC(0, UA_NS0ID_BASEOBJECTTYPE)) &&
		   !UA_NodeId_equal(&typeNodeId, &UA_NODEID_NUMERIC(0, UA_NS0ID_BASEVARIABLETYPE)))
	{
		QList<UA_NodeId> childrenNodeIds = QUaNode::getChildrenNodeIds(typeNodeId, m_qUaServer->m_server);
		for (auto childNodeId : childrenNodeIds)
		{
			// ignore if not optional
			if (!QUaNode::hasOptionalModellingRule(childNodeId, m_qUaServer->m_server))
			{
				UA_NodeId_clear(&childNodeId);
				continue;
			}
			// ignore if browse name does not match
			QUaQualifiedName childBrowseName = QUaNode::getBrowseName(childNodeId, m_qUaServer->m_server);
			if (childBrowseName != browseName)
			{
				UA_NodeId_clear(&childNodeId);
				continue;
			}
			// copy, do not clear because will be used later
			optionalFieldNodeId = childNodeId;
		}
		// cleanup
		for (auto childNodeId : childrenNodeIds)
		{
			UA_NodeId_clear(&childNodeId);
		}
		// check if found
		if (!UA_NodeId_isNull(&optionalFieldNodeId))
		{
			break;
		}
		UA_NodeId typeNodeIdNew = QUaNode::superTypeDefinitionNodeId(typeNodeId, m_qUaServer->m_server);
		UA_NodeId_clear(&typeNodeId);
		UA_NodeId_copy(&typeNodeIdNew, &typeNodeId);
		UA_NodeId_clear(&typeNodeIdNew);
	}
	if (UA_NodeId_isNull(&optionalFieldNodeId))
	{
		UA_LOG_WARNING(&m_qUaServer->m_server->config.logger, UA_LOGCATEGORY_USERLAND,
			"Couldn't find optional Field Node in ConditionType. StatusCode %s",
			UA_StatusCode_name(UA_STATUSCODE_BADNOTFOUND));
		return nullptr;
	}
	// get the internal node
	const UA_Node* optionalFieldNode = UA_NODESTORE_GET(m_qUaServer->m_server, &optionalFieldNodeId);
	UA_NodeId_clear(&optionalFieldNodeId);
	if (optionalFieldNode == NULL) 
	{
		UA_LOG_WARNING(&m_qUaServer->m_server->config.logger, UA_LOGCATEGORY_USERLAND,
			"Couldn't find optional Field Node in ConditionType. StatusCode %s",
			UA_StatusCode_name(UA_STATUSCODE_BADNOTFOUND));
		return nullptr;
	}
	// convert browse name to qualified name
	UA_QualifiedName fieldName = browseName;
	// instantiate according to type
	UA_NodeId outOptionalNode;
	UA_NodeClass nodeClass = optionalFieldNode->nodeClass;
	switch (nodeClass) {
	case UA_NODECLASS_VARIABLE: 
	{
		UA_StatusCode retval = QUaNode::addOptionalVariableField(
			m_qUaServer->m_server, 
			&m_nodeId, 
			&fieldName,
			(const UA_VariableNode*)optionalFieldNode, 
			&outOptionalNode
		);
		if (retval != UA_STATUSCODE_GOOD) 
		{
			UA_LOG_ERROR(&m_qUaServer->m_server->config.logger, UA_LOGCATEGORY_USERLAND,
				"Adding Condition Optional Variable Field failed. StatusCode %s",
				UA_StatusCode_name(retval));
			UA_QualifiedName_deleteMembers(&fieldName);
			return nullptr;
		}
		UA_NODESTORE_RELEASE(m_qUaServer->m_server, optionalFieldNode);
	}
	break;
	case UA_NODECLASS_OBJECT: 
	{
		UA_StatusCode retval = QUaNode::addOptionalObjectField(
			m_qUaServer->m_server, 
			&m_nodeId, &fieldName,
			(const UA_ObjectNode*)optionalFieldNode, 
			&outOptionalNode
		);
		if (retval != UA_STATUSCODE_GOOD) 
		{
			UA_LOG_ERROR(&m_qUaServer->m_server->config.logger, UA_LOGCATEGORY_USERLAND,
				"Adding Condition Optional Object Field failed. StatusCode %s",
				UA_StatusCode_name(retval));
			UA_QualifiedName_deleteMembers(&fieldName);
			return nullptr;
		}
		UA_NODESTORE_RELEASE(m_qUaServer->m_server, optionalFieldNode);
	}
	break;
	case UA_NODECLASS_METHOD:
		// NOTE : use QUaNode::addOptionalMethod instead
		UA_QualifiedName_deleteMembers(&fieldName);
		UA_NODESTORE_RELEASE(m_qUaServer->m_server, optionalFieldNode);
		return nullptr;
	default:
		UA_QualifiedName_deleteMembers(&fieldName);
		UA_NODESTORE_RELEASE(m_qUaServer->m_server, optionalFieldNode);
		return nullptr;
	}
	UA_QualifiedName_deleteMembers(&fieldName);
	// trigger reference added, model change event, so client (UaExpert) auto refreshes tree
#ifdef UA_ENABLE_SUBSCRIPTIONS_EVENTS
	if (!this->metaObject()->inherits(&QUaBaseEvent::staticMetaObject)
#ifdef UA_ENABLE_SUBSCRIPTIONS_ALARMS_CONDITIONS
		|| this->metaObject()->inherits(&QUaCondition::staticMetaObject)
#endif // UA_ENABLE_SUBSCRIPTIONS_ALARMS_CONDITIONS
		)
	{
		// add reference added change to buffer
		m_qUaServer->addChange({
			this->nodeId(),
			this->typeDefinitionNodeId(),
			QUaChangeVerb::ReferenceAdded // UaExpert does not recognize QUaChangeVerb::NodeAdded
		});
	}
#endif // UA_ENABLE_SUBSCRIPTIONS_EVENTS
	// if we reached here, the optional field has been instantiated
	// if the type of the optional field is registered in QUaServer then
	// it has been boud to its corresponding Qt type and there is nothing
	// more to do here
	QUaNode* child = QUaNode::getNodeContext(outOptionalNode, m_qUaServer->m_server);
	if (child)
	{
		Q_ASSERT(child->browseName() == browseName);
		// TODO : clean up ?
		return child;
	}
	// else the type is not registered, so we need to create an instance of its
	// base class at least
	QMetaObject metaObject;
	switch (nodeClass) {
	case UA_NODECLASS_VARIABLE: {
		metaObject = QUaBaseDataVariable::staticMetaObject;
	}
	case UA_NODECLASS_OBJECT: {
		metaObject = QUaBaseObject::staticMetaObject;
	}
	default:
		Q_ASSERT(false);
		return nullptr;
	}
	// to understand code below, see QUaServer::uaConstructor
	m_qUaServer->m_newNodeNodeId     = &outOptionalNode;
	m_qUaServer->m_newNodeMetaObject = &metaObject;
	// instantiate new C++ node, m_newNodeNodeId and m_newNodeMetaObject only meant to be used during this call
	auto* pQObject = metaObject.newInstance(Q_ARG(QUaServer*, m_qUaServer));
	Q_ASSERT_X(pQObject, "QUaNode::instantiateOptionalChild",
		"Failed instantiation. No matching Q_INVOKABLE constructor with signature "
		"CONSTRUCTOR(QUaServer *server) found.");
	auto* newInstance = qobject_cast<QUaNode*>(pQObject);
	Q_CHECK_PTR(newInstance);
	if (!newInstance)
	{
		return nullptr;
	}
	// need to bind again using the official (void ** nodeContext) of the UA constructor
	// because we set context on C++ instantiation, but later the UA library overwrites it 
	// after calling the UA constructor
	auto st = UA_Server_setNodeContext(
		m_qUaServer->m_server,
		outOptionalNode,
		static_cast<void*>(newInstance)
	);
	Q_ASSERT(st);
	Q_UNUSED(st);
	newInstance->m_nodeId = outOptionalNode;
	// need to set parent and browse name
	newInstance->setParent(this);
	newInstance->setObjectName( browseName);
	// emit child added to parent
	emit this->childAdded(newInstance);
	// success
	return newInstance;
}

QUaNode* QUaNode::cloneNode(
	QUaNode* parentNode/* = nullptr*/,
	const QUaQualifiedName&  browseName/* = ""*/,
	const QUaNodeId& nodeId/* = ""*/
)
{
	// create new clean instance of the same type
	UA_NodeId newInstanceNodeId = m_qUaServer->createInstanceInternal(
		*this->metaObject(),
		parentNode,
		browseName.isEmpty() ? this->browseName() :  browseName,
		nodeId
	);
	if (UA_NodeId_isNull(&newInstanceNodeId))
	{
		UA_NodeId_clear(&newInstanceNodeId);
		return nullptr;
	}
	// get new c++ instance
	auto tmp = QUaNode::getNodeContext(newInstanceNodeId, m_qUaServer);
	QUaNode* newInstance = qobject_cast<QUaNode*>(tmp);
	Q_CHECK_PTR(newInstance);
	UA_NodeId_clear(&newInstanceNodeId);
	if (!newInstance)
	{
		return newInstance;
	}
	// serialize this instance to memory
	QQueue<QUaLog> logOut;
	QUaInMemorySerializer serializer;
	this->serialize(serializer, logOut);

	//// [DEBUG]
	//serializer.qtDebug(
	//	m_qUaServer, 
	//	tr("*************** %1 ***************").arg(this->nodeId())
	//);

	// replace nodeId
	Q_ASSERT(serializer.m_hashNodeTreeData.contains(this->nodeId()));
	serializer.m_hashNodeTreeData[newInstance->nodeId()] =
		serializer.m_hashNodeTreeData.take(this->nodeId());
	// replace browseName
	Q_ASSERT(serializer.m_hashNodeTreeData[newInstance->nodeId()].attrs.contains("browseName"));
	serializer.m_hashNodeTreeData[newInstance->nodeId()].attrs["browseName"] = 
		newInstance->browseName().toXmlString();
	// deserialize to new instance
	newInstance->deserialize(serializer, logOut);

	//// [DEBUG]
	////serializer.clear();
	//newInstance->serialize(serializer, logOut);
	//serializer.qtDebug(
	//	m_qUaServer, 
	//	tr("*************** %1 ***************").arg(this->nodeId())
	//);
	// return new instance
	return newInstance;
}

const QUaSession* QUaNode::currentSession() const
{
	return m_qUaServer->m_currentSession;
}

bool QUaNode::hasOptionalMethod(const QUaQualifiedName& methodName) const
{
	// get all ua methods of INSTANCE
	auto methodsNodeIds = QUaNode::getMethodsNodeIds(m_nodeId, m_qUaServer->m_server);
	for (auto methNodeId : methodsNodeIds)
	{
		// ignore if not optional
		if (!QUaNode::hasOptionalModellingRule(methNodeId, m_qUaServer->m_server))
		{
			continue;
		}
		// ignore if browse name match
		QUaQualifiedName methBrowseName = QUaNode::getBrowseName(methNodeId, m_qUaServer->m_server);
		if (methodName == methBrowseName)
		{
			// cleanup
			for (int i = 0; i < methodsNodeIds.count(); i++)
			{
				UA_NodeId_clear(&methodsNodeIds[i]);
			}
			return true;
		}
	}
	// cleanup
	for (int i = 0; i < methodsNodeIds.count(); i++)
	{
		UA_NodeId_clear(&methodsNodeIds[i]);
	}
	return false;
}

bool QUaNode::addOptionalMethod(const QUaQualifiedName& methodName)
{
	// do not add twice
	if (this->hasOptionalMethod(methodName))
	{
		return true;
	}
	UA_NodeId typeNodeId = QUaNode::typeDefinitionNodeId(m_nodeId, m_qUaServer->m_server);
	UA_NodeId methodNodeId = UA_NODEID_NULL;
	// look for optional method starting from this type of to base object type
	while (!UA_NodeId_equal(&typeNodeId, &UA_NODEID_NUMERIC(0, UA_NS0ID_BASEOBJECTTYPE)))
		{
		// get all ua methods of TYPE
		auto methodsNodeIds = QUaNode::getMethodsNodeIds(typeNodeId, m_qUaServer->m_server);
		for (auto methNodeId : methodsNodeIds)
		{
			// ignore if not optional
			if (!QUaNode::hasOptionalModellingRule(methNodeId, m_qUaServer->m_server))
			{
				continue;
			}
			// ignore if browse name does not match
			QUaQualifiedName methBrowseName = QUaNode::getBrowseName(methNodeId, m_qUaServer->m_server);
			if (methodName != methBrowseName)
			{
				continue;
			}
			methodNodeId = methNodeId;
			break;
		}
		// cleanup
		for (auto methNodeId : methodsNodeIds)
		{
			UA_NodeId_clear(&methNodeId);
		}
		// check if found
		if (!UA_NodeId_isNull(&methodNodeId))
		{
			break;
		}
		UA_NodeId typeNodeIdNew = QUaNode::superTypeDefinitionNodeId(typeNodeId, m_qUaServer->m_server);
		UA_NodeId_clear(&typeNodeId);
		UA_NodeId_copy(&typeNodeIdNew, &typeNodeId);
		UA_NodeId_clear(&typeNodeIdNew);
	}
	UA_NodeId_clear(&typeNodeId);
	Q_ASSERT_X(!UA_NodeId_isNull(&methodNodeId),
		"QUaNode::addOptionalMethod", "Could not find optional method.");
	if (UA_NodeId_isNull(&methodNodeId))
	{
		return false;
	}
	// add reference from instance to method
	auto st = UA_Server_addReference(
		m_qUaServer->m_server,
		m_nodeId,
		UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT),
		{ methodNodeId, UA_STRING_NULL, 0 },
		true
	);
	Q_ASSERT(st == UA_STATUSCODE_GOOD);
	if (st != UA_STATUSCODE_GOOD)
	{
		return false;
	}
	return true;
}

bool QUaNode::removeOptionalMethod(const QUaQualifiedName& methodName)
{
	// do not remove twice
	if (!this->hasOptionalMethod(methodName))
	{
		return true;
	}
	UA_NodeId typeNodeId = QUaNode::typeDefinitionNodeId(m_nodeId, m_qUaServer->m_server);
	// get all ua methods of TYPE
	auto methodsNodeIds = QUaNode::getMethodsNodeIds(typeNodeId, m_qUaServer->m_server);
	UA_NodeId methodNodeId = UA_NODEID_NULL;
	for (auto methNodeId : methodsNodeIds)
	{
		// ignore if not optional
		if (!QUaNode::hasOptionalModellingRule(methNodeId, m_qUaServer->m_server))
		{
			continue;
		}
		// ignore if browse name does not match
		QUaQualifiedName methBrowseName = QUaNode::getBrowseName(methNodeId, m_qUaServer->m_server);
		if (methodName != methBrowseName)
		{
			continue;
		}
		methodNodeId = methNodeId;
		break;
	}
	Q_ASSERT_X(!UA_NodeId_isNull(&methodNodeId),
		"QUaNode::addOptionalMethod", "Could not find optional method.");
	if (UA_NodeId_isNull(&methodNodeId))
	{
		// cleanup
		for (int i = 0; i < methodsNodeIds.count(); i++)
		{
			UA_NodeId_clear(&methodsNodeIds[i]);
		}
		UA_NodeId_clear(&typeNodeId);
		return false;
	}
	// remove reference from instance to method
	auto st = UA_Server_deleteReference(
		m_qUaServer->m_server,
		m_nodeId,
		UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT),
		true,
		{ methodNodeId, UA_STRING_NULL, 0 },
		true
	);
	// cleanup
	for (int i = 0; i < methodsNodeIds.count(); i++)
	{
		UA_NodeId_clear(&methodsNodeIds[i]);
	}
	UA_NodeId_clear(&typeNodeId);
	Q_ASSERT(st == UA_STATUSCODE_GOOD);
	if (st != UA_STATUSCODE_GOOD)
	{
		return false;
	}
	return true;
}

const QMap<QString, QVariant> QUaNode::serializeAttrs() const
{
	QMap<QString, QVariant> retMap;
	// first serialize browseName
	retMap["browseName"] = this->browseName().toXmlString();
	// list meta props
	auto metaObject = this->metaObject();
	int  propCount  = metaObject->propertyCount();
	int  propOffset = QUaNode::staticMetaObject.propertyOffset();
	for (int i = propOffset; i < propCount; i++)
	{
		QMetaProperty metaProperty = metaObject->property(i);
		QString strPropName = QString(metaProperty.name());
		Q_ASSERT(metaProperty.isReadable());
		// non-writabe props cannot be restored
		if (!metaProperty.isWritable())
		{
			continue;
		}
		Q_ASSERT(!retMap.contains(strPropName));
		// ignore QUaNode * properties (type children)
		int type = metaProperty.userType();
		auto propMetaObject = QMetaType::metaObjectForType(type);
		if (propMetaObject && propMetaObject->inherits(&QUaNode::staticMetaObject))
		{
			continue;
		}
		QVariant value = metaProperty.read(this);
		// check known type
		auto metaType = static_cast<QMetaType::Type>(type);
		if (metaType == QMetaType::UnknownType)
		{
			continue;
		}
		// get the Qt meta property name
		retMap[strPropName] = value;
	}
	return retMap;
}

const QList<QUaForwardReference> QUaNode::serializeRefs() const
{
	QList<QUaForwardReference> retList;
	// serialize all reference types
	for (auto& refType : m_qUaServer->referenceTypes())
	{
		for (auto ref : this->findReferences(refType))
		{
			QUaForwardReference fRef = { 
				ref->nodeId(), 
				ref->metaObject()->className(),
				refType 
			};
			retList << fRef;
		}
	}
	return retList;
}

void QUaNode::deserializeAttrs(const QMap<QString, QVariant>& attrs, QQueue<QUaLog>& logOut)
{
	QStringList listAttrsNotInProps = attrs.keys();
	QStringList listPropsNotInAttrs;
	// first deserialize browseName
	Q_ASSERT_X(
		attrs.contains("browseName"), 
		"QUaNode::deserializeAttrs", 
		"Deserialized attributes must contain the browseName"
	);
	Q_ASSERT_X(
		QUaQualifiedName::fromXmlString(attrs["browseName"].toString()) == this->browseName(),
		"QUaNode::deserializeAttrs",
		"Deserialized browseName does not match instance browseName"
	);
	listAttrsNotInProps.removeOne("browseName");
	// list meta props
	auto metaObject = this->metaObject();
	int  propCount  = metaObject->propertyCount();
	int  propOffset = QUaNode::staticMetaObject.propertyOffset();
	for (int i = propOffset; i < propCount; i++)
	{
		QMetaProperty metaProperty = metaObject->property(i);
		QString strPropName = QString(metaProperty.name());
		Q_ASSERT(metaProperty.isReadable());
		// non-writabe props cannot be restored
		if (!metaProperty.isWritable())
		{
			continue;
		}
		// ignore QUaNode * properties (type children)
		int type = metaProperty.userType();
		auto propMetaObject = QMetaType::metaObjectForType(type);
		if (propMetaObject && propMetaObject->inherits(&QUaNode::staticMetaObject))
		{
			continue;
		}
		// check known type
		auto metaType = static_cast<QMetaType::Type>(type);
		if (metaType == QMetaType::UnknownType)
		{
			continue;
		}
		// check if metaprop in attrs
		if (attrs.contains(strPropName))
		{
			// remove from shame list
			bool ok = listAttrsNotInProps.removeOne(strPropName);
			Q_ASSERT(ok);
			// write property
			auto val = attrs[strPropName];
			if (val.isValid() && !val.isNull())
			{
				ok = metaProperty.write(this, val);
				Q_ASSERT_X(ok, "QUaNode::deserializeAttrs", "Invalid value for deserialized attribute.");
			}
			continue;
		}
		// else add to shame list
		listPropsNotInAttrs << strPropName;
	}
	// check props not in attrs
	if (!listPropsNotInAttrs.isEmpty())
	{
		logOut.enqueue({
			tr("Node %1 has attributes that were not found in data; %2. Ignoring.")
				.arg(this->nodeId())
				.arg(listPropsNotInAttrs.join(", ")),
			QUaLogLevel::Warning,
			QUaLogCategory::Serialization
		});
	}
	// check attrs not in props
	if (!listAttrsNotInProps.isEmpty())
	{
		logOut.enqueue({
			tr("Node %1 does not have attributes that were found in data; %2. Ignoring.")
				.arg(this->nodeId())
				.arg(listAttrsNotInProps.join(", ")),
			QUaLogLevel::Warning,
			QUaLogCategory::Serialization
		});
	}
}

QUaWriteMask QUaNode::userWriteMask(const QString & strUserName)
{
	// if has a node parent, use parent's callback
	QUaNode * parent = qobject_cast<QUaNode*>(this->parent());
	if (parent)
	{
		return parent->userWriteMask(strUserName);
	}
	// else all permissions
	return 0xFFFFFFFF;
}

QUaAccessLevel QUaNode::userAccessLevel(const QString & strUserName)
{
	// if has a node parent, use parent's callback
	QUaNode * parent = qobject_cast<QUaNode*>(this->parent());
	if (parent)
	{
		return parent->userAccessLevelInternal(strUserName);
	}
	// else all permissions
	return 0xFF;
}

bool QUaNode::userExecutable(const QString & strUserName)
{
	// if has a node parent, use parent's callback
	QUaNode * parent = qobject_cast<QUaNode*>(this->parent());
	if (parent)
	{
		return parent->userExecutable(strUserName);
	}
	// else allow
	return true;
}

QString QUaNode::className() const
{
	return QString(this->metaObject()->className());
}

// NOTE : need to cleanup result after calling this method
UA_NodeId QUaNode::getParentNodeId(const UA_NodeId & childNodeId, QUaServer * server)
{
	return QUaNode::getParentNodeId(childNodeId, server->m_server);
}

// NOTE : need to cleanup result after calling this method
UA_NodeId QUaNode::getParentNodeId(const UA_NodeId & childNodeId, UA_Server * server)
{
	UA_BrowseDescription * bDesc = UA_BrowseDescription_new();
	UA_NodeId_copy(&childNodeId, &bDesc->nodeId); // from child
	bDesc->browseDirection = UA_BROWSEDIRECTION_INVERSE; //  look upwards
	bDesc->includeSubtypes = true;
	bDesc->resultMask      = UA_BROWSERESULTMASK_BROWSENAME | UA_BROWSERESULTMASK_DISPLAYNAME;
	// browse
	UA_BrowseResult bRes = UA_Server_browse(server, 0, bDesc);
	Q_ASSERT(bRes.statusCode == UA_STATUSCODE_GOOD);
	QList<UA_NodeId> listParents;
	while (bRes.referencesSize > 0)
	{
		for (size_t i = 0; i < bRes.referencesSize; i++)
		{
			UA_ReferenceDescription rDesc = bRes.references[i];
			// NOTE : it seems cleanup below also deletes the strings of string nodeIds which creates a bug
			//        this means we need to cleanup the result everytime we call QUaNode::getParentNodeId
			//UA_NodeId nodeId = rDesc.nodeId.nodeId;
			UA_NodeId nodeId;
			UA_NodeId_copy(&rDesc.nodeId.nodeId, &nodeId);
			listParents.append(nodeId);
		}
		UA_BrowseResult_deleteMembers(&bRes);
		bRes = UA_Server_browseNext(server, true, &bRes.continuationPoint);
	}
	// cleanup
	UA_BrowseDescription_deleteMembers(bDesc);
	UA_BrowseDescription_delete(bDesc);
	UA_BrowseResult_deleteMembers(&bRes);
	// check if method
	UA_NodeClass outNodeClass;
	UA_Server_readNodeClass(server, childNodeId, &outNodeClass);
	// NOTE : seems methods added to subtype have references to all instances created of the subtype
	// TODO : fix, when https://github.com/open62541/open62541/pull/1812 is fixed
	Q_ASSERT_X(
		(listParents.count() <= 1 && outNodeClass != UA_NODECLASS_METHOD) ||
		(listParents.count() >= 1 && outNodeClass == UA_NODECLASS_METHOD),
		"QUaServer::getParentNodeId", "Child code it not supposed to have more than one parent.");
	// return
	return listParents.count() > 0 ? listParents.at(0) : UA_NODEID_NULL;
}

// NOTE : need to cleanup result after calling this method
QList<UA_NodeId> QUaNode::getChildrenNodeIds(const UA_NodeId & parentNodeId, QUaServer * server)
{
	return QUaNode::getChildrenNodeIds(parentNodeId, server->m_server);
}

// NOTE : need to cleanup result after calling this method
QList<UA_NodeId> QUaNode::getChildrenNodeIds(const UA_NodeId & parentNodeId, UA_Server * server)
{
	QList<UA_NodeId> retListChildren;
	UA_BrowseDescription * bDesc = UA_BrowseDescription_new();
	UA_NodeId_copy(&parentNodeId, &bDesc->nodeId); // from parent
	bDesc->browseDirection = UA_BROWSEDIRECTION_FORWARD; //  look downwards
	bDesc->includeSubtypes = false;
	bDesc->nodeClassMask   = UA_NODECLASS_OBJECT | UA_NODECLASS_VARIABLE; // only objects or variables (no types or refs)
	bDesc->resultMask      = UA_BROWSERESULTMASK_NONE; // only need node ids
	// browse
	UA_BrowseResult bRes = UA_Server_browse(server, 0, bDesc);
	assert(bRes.statusCode == UA_STATUSCODE_GOOD);
	while (bRes.referencesSize > 0)
	{
		for (size_t i = 0; i < bRes.referencesSize; i++)
		{
			UA_ReferenceDescription rDesc = bRes.references[i];
			UA_NodeId nodeId/* = rDesc.nodeId.nodeId*/;
			UA_NodeId_copy(&rDesc.nodeId.nodeId, &nodeId);
			// ignore modelling rules
            auto nodeIdMandatory = UA_NODEID_NUMERIC(0, UA_NS0ID_MODELLINGRULE_MANDATORY);
            if (UA_NodeId_equal(&nodeId, &nodeIdMandatory))
			{
				UA_NodeId_clear(&nodeId);
				continue;
			}
			retListChildren.append(nodeId);
		}
		UA_BrowseResult_deleteMembers(&bRes);
		bRes = UA_Server_browseNext(server, true, &bRes.continuationPoint);
	}
	// cleanup
	UA_BrowseDescription_deleteMembers(bDesc);
	UA_BrowseDescription_delete(bDesc);
	UA_BrowseResult_deleteMembers(&bRes);
	// return
	return retListChildren;
}

// NOTE : need to cleanup result after calling this method
QList<UA_NodeId> QUaNode::getMethodsNodeIds(const UA_NodeId& parentNodeId, QUaServer* server)
{
	return QUaNode::getMethodsNodeIds(parentNodeId, server->m_server);
}

// NOTE : need to cleanup result after calling this method
QList<UA_NodeId> QUaNode::getMethodsNodeIds(const UA_NodeId& parentNodeId, UA_Server* server)
{
	QList<UA_NodeId> retListMethods;
	UA_BrowseDescription* bDesc = UA_BrowseDescription_new();
	UA_NodeId_copy(&parentNodeId, &bDesc->nodeId); // from parent
	bDesc->browseDirection = UA_BROWSEDIRECTION_FORWARD; //  look downwards
	bDesc->includeSubtypes = false;
	bDesc->nodeClassMask = UA_NODECLASS_METHOD; // only methods
	bDesc->resultMask = UA_BROWSERESULTMASK_NONE; // only need node ids
	// browse
	UA_BrowseResult bRes = UA_Server_browse(server, 0, bDesc);
	assert(bRes.statusCode == UA_STATUSCODE_GOOD);
	while (bRes.referencesSize > 0)
	{
		for (size_t i = 0; i < bRes.referencesSize; i++)
		{
			UA_ReferenceDescription rDesc = bRes.references[i];
			UA_NodeId nodeId/* = rDesc.nodeId.nodeId*/;
			UA_NodeId_copy(&rDesc.nodeId.nodeId, &nodeId);
			retListMethods.append(nodeId);
		}
		UA_BrowseResult_deleteMembers(&bRes);
		bRes = UA_Server_browseNext(server, true, &bRes.continuationPoint);
	}
	// cleanup
	UA_BrowseDescription_deleteMembers(bDesc);
	UA_BrowseDescription_delete(bDesc);
	UA_BrowseResult_deleteMembers(&bRes);
	// return
	return retListMethods;
}

QUaNode * QUaNode::getNodeContext(const UA_NodeId & nodeId, QUaServer * server)
{
	return QUaNode::getNodeContext(nodeId, server->m_server);
}

QUaNode * QUaNode::getNodeContext(const UA_NodeId & nodeId, UA_Server * server)
{
	void * context = QUaNode::getVoidContext(nodeId, server);
	// try to cast to C++ node, dynamic_cast check is necessary
	return qobject_cast<QUaNode*>(static_cast<QObject*>(context));
}

void * QUaNode::getVoidContext(const UA_NodeId & nodeId, UA_Server * server)
{
	// get void context
	void * context;
	auto st = UA_Server_getNodeContext(server, nodeId, &context);
	if (st != UA_STATUSCODE_GOOD)
	{
		return nullptr;
	}
	// try to cast to C++ node
	return context;
}

QUaQualifiedName QUaNode::getBrowseName(const UA_NodeId & nodeId, QUaServer * server)
{
	return QUaNode::getBrowseName(nodeId, server->m_server);
}

QUaQualifiedName QUaNode::getBrowseName(const UA_NodeId & nodeId, UA_Server * server)
{
	// read browse name
	UA_QualifiedName outBrowseName;
	auto st = UA_Server_readBrowseName(server, nodeId, &outBrowseName);
	Q_ASSERT(st == UA_STATUSCODE_GOOD);
	Q_UNUSED(st);
	QUaQualifiedName  browseName = outBrowseName;
	UA_QualifiedName_clear(&outBrowseName);
	return  browseName;
}

bool QUaNode::hasMandatoryModellingRule(const UA_NodeId& nodeId, QUaServer* server)
{
	return QUaNode::hasMandatoryModellingRule(nodeId, server->m_server);
}

bool QUaNode::hasMandatoryModellingRule(const UA_NodeId& nodeId, UA_Server* server)
{
	UA_BrowseDescription * bDesc = UA_BrowseDescription_new();
	UA_NodeId_copy(&nodeId, &bDesc->nodeId); // from parent
	bDesc->referenceTypeId = UA_NODEID_NUMERIC(0, UA_NS0ID_HASMODELLINGRULE);
	bDesc->browseDirection = UA_BROWSEDIRECTION_FORWARD; //  look downwards
	bDesc->includeSubtypes = false;
	bDesc->nodeClassMask   = UA_NODECLASS_OBJECT; // in specific UA_NS0ID_MODELLINGRULE_MANDATORY 78 /* Object */
	bDesc->resultMask      = UA_BROWSERESULTMASK_NONE; // no info needed, only existance
	// browse
	UA_BrowseResult bRes = UA_Server_browse(server, 0, bDesc);
	Q_ASSERT(bRes.statusCode == UA_STATUSCODE_GOOD);
	bool isMandatory = false;
	if (bRes.referencesSize > 0)
	{
		Q_ASSERT(bRes.referencesSize == 1);
		// check if modelling rule is mandatory
		UA_ReferenceDescription rDesc = bRes.references[0];
		isMandatory = UA_NodeId_equal(
			&rDesc.nodeId.nodeId, 
			&UA_NODEID_NUMERIC(0, UA_NS0ID_MODELLINGRULE_MANDATORY)
		);		
	}
	// cleanup
	UA_BrowseDescription_deleteMembers(bDesc);
	UA_BrowseDescription_delete(bDesc);
	UA_BrowseResult_deleteMembers(&bRes);
	// return
	return isMandatory;
}

bool QUaNode::hasOptionalModellingRule(const UA_NodeId& nodeId, QUaServer* server)
{
	return QUaNode::hasOptionalModellingRule(nodeId, server->m_server);
}

bool QUaNode::hasOptionalModellingRule(const UA_NodeId& nodeId, UA_Server* server)
{
	UA_BrowseDescription* bDesc = UA_BrowseDescription_new();
	UA_NodeId_copy(&nodeId, &bDesc->nodeId); // from parent
	bDesc->referenceTypeId = UA_NODEID_NUMERIC(0, UA_NS0ID_HASMODELLINGRULE);
	bDesc->browseDirection = UA_BROWSEDIRECTION_FORWARD; //  look downwards
	bDesc->includeSubtypes = false;
	bDesc->nodeClassMask   = UA_NODECLASS_OBJECT; // in specific UA_NS0ID_MODELLINGRULE_OPTIONAL 80 /* Object */
	bDesc->resultMask      = UA_BROWSERESULTMASK_NONE; // no info needed, only existance
	// browse
	UA_BrowseResult bRes = UA_Server_browse(server, 0, bDesc);
	Q_ASSERT(bRes.statusCode == UA_STATUSCODE_GOOD);
	bool isMandatory = false;
	if (bRes.referencesSize > 0)
	{
		Q_ASSERT(bRes.referencesSize == 1);
		// check if modelling rule is mandatory
		UA_ReferenceDescription rDesc = bRes.references[0];
		isMandatory = UA_NodeId_equal(
			&rDesc.nodeId.nodeId,
			&UA_NODEID_NUMERIC(0, UA_NS0ID_MODELLINGRULE_OPTIONAL)
		);
	}
	// cleanup
	UA_BrowseDescription_deleteMembers(bDesc);
	UA_BrowseDescription_delete(bDesc);
	UA_BrowseResult_deleteMembers(&bRes);
	// return
	return isMandatory;
}

int QUaNode::getPropsOffsetHelper(const QMetaObject & metaObject)
{
	int propOffset;
	// need to set props also inherited from base class
	if (metaObject.inherits(&QUaBaseVariable::staticMetaObject))
	{
		propOffset = QUaBaseVariable::staticMetaObject.propertyOffset();
	}
	else
	{
		// [NOTE] : assert below does not work, dont know why
		//Q_ASSERT(metaObject.inherits(&QUaBaseObject::staticMetaObject));
		propOffset = QUaBaseObject::staticMetaObject.propertyOffset();
	}
	return propOffset;
}

QDebug operator<<(QDebug debug, const QUaReferenceType& refType)
{
	QDebugStateSaver saver(debug);
	debug.nospace() << '{' << refType.strForwardName << ", " << refType.strInverseName << '}';
	return debug;
}

QUaLog::QUaLog()
{
	// default constructor required by Qt
	message   = QByteArray();
	timestamp = QDateTime::currentDateTimeUtc();
}

QUaLog::QUaLog(const QString        & strMessage,
	           const QUaLogLevel    & logLevel, 
	           const QUaLogCategory & logCategory)
{
	message   = strMessage.toUtf8();
	level     = logLevel;
	category  = logCategory;
	timestamp = QDateTime::currentDateTimeUtc();
}
