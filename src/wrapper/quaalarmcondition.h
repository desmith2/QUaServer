#ifndef QUAALARMCONDITION_H
#define QUAALARMCONDITION_H

#include <QUaAcknowledgeableCondition>

#ifdef UA_ENABLE_SUBSCRIPTIONS_ALARMS_CONDITIONS

class QUaAlarmCondition : public QUaAcknowledgeableCondition
{
    Q_OBJECT

friend class QUaServer;

public:
	Q_INVOKABLE explicit QUaAlarmCondition(
		QUaServer *server
	);

	// children

	// If condition currently exists
	QString   activeStateCurrentStateName() const;
	void      setActiveStateCurrentStateName(const QString& activeState);
	bool      activeStateId() const;
	void      setActiveStateId(const bool& activeStateId);
	QDateTime activeStateTransitionTime() const;
	void      setActiveStateTransitionTime(const QDateTime& transitionTime);
	QString   activeStateTrueState() const;
	void      setActiveStateTrueState(const QString& trueState);
	QString   activeStateFalseState() const;
	void      setActiveStateFalseState(const QString& falseState);
	// helper
	bool active() const;
	void setActive(const bool& active);

	// NodeId of the Variable which is used for the calculation of the Alarm state.
	// Can be null if internal
	QUaNodeId inputNode() const;
	void      setInputNode(const QUaNodeId& QUaNodeId);

	// TODO : SuppressedState
	// TODO : OutOfServiceState
	// TODO : ShelvingState

	bool suppressedOrShelve() const;
	void setSuppressedOrShelve(const bool& suppressedOrShelve);

	// TODO : MaxTimeShelved
	// TODO : AudibleEnabled
	// TODO : AudibleSound
	// TODO : SilenceState
	// TODO : OnDelay
	// TODO : OffDelay
	// TODO : FirstInGroupFlag
	// TODO : FirstInGroup
	// TODO : LatchedState
	// TODO : <AlarmGroup>
	// TODO : ReAlarmTime
	// TODO : ReAlarmRepeatCount

	// methods

	Q_INVOKABLE void Silence();
	
	Q_INVOKABLE void Suppress();
	
	Q_INVOKABLE void Unsuppress();
	
	Q_INVOKABLE void RemoveFromService();
	
	Q_INVOKABLE void PlaceInService();
	
	Q_INVOKABLE void Reset();


signals:
	void activated();
	void deactivated();


protected:
	// LocalizedText
	QUaTwoStateVariable* getActiveState();
	// NodeId
	QUaProperty* getInputNode();

	// TODO : LocalizedText, getSuppressedState
	// TODO : LocalizedText, getOutOfServiceState
	// TODO : ShelvedStateMachineType, getShelvingState

	// Boolean
	QUaProperty* getSuppressedOrShelve();

	// TODO : Duration, getMaxTimeShelved

	// TODO : Boolean, getAudibleEnabled
	// TODO : AudioDataType, AudibleSound

	// TODO : LocalizedText, getSilenceState

	// TODO : Duration, getOnDelay
	// TODO : Duration, getOffDelay

	// TODO : Boolean, getFirstInGroupFlag
	// TODO : AlarmGroupType, getFirstInGroup
	// TODO : LocalizedText, getLatchedState

	// TODO : AlarmGroupType, get<AlarmGroup>

	// TODO : Duration, getReAlarmTime
	// TODO : Int16, getReAlarmRepeatCount

	// helpers

	// reimplement to define branch delete conditions
	virtual bool canDeleteBranch() const;
	// reimplement to reset type internals (QUaAlarmCondition::Reset)
	virtual void resetInternals();

};

#endif // UA_ENABLE_SUBSCRIPTIONS_ALARMS_CONDITIONS

#endif // QUAALARMCONDITION_H