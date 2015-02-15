/*
* Copyright (c) 2013 Jonathan Perkin <jonathan@perkin.org.uk>
*
* Permission to use, copy, modify, and distribute this software for any
* purpose with or without fee is hereby granted, provided that the above
* copyright notice and this permission notice appear in all copies.
*
* THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
* WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
* MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
* ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
* WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
* ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
* OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/
#include <iostream>



#include <list>
#include <queue>

#include <node.h>
#include <v8.h>

#include "Manager.h"
#include "Node.h"
#include "Notification.h"
#include "Options.h"
#include "Value.h"

#ifdef WIN32
class mutex
{
public:
	mutex()              { InitializeCriticalSection(&_criticalSection); }
	~mutex()             { DeleteCriticalSection(&_criticalSection); }
	inline void lock()   { EnterCriticalSection(&_criticalSection); }
	inline void unlock() { LeaveCriticalSection(&_criticalSection); }

	class scoped_lock
	{
	public:
		inline explicit scoped_lock(mutex & sp) : _sl(sp) { _sl.lock(); }
		inline ~scoped_lock()                             { _sl.unlock(); }
	private:
		scoped_lock(scoped_lock const &);
		scoped_lock & operator=(scoped_lock const &);
		mutex& _sl;
	};

private:
	CRITICAL_SECTION _criticalSection;
};
#endif

#ifdef linux
#include <unistd.h>
#include <pthread.h>

class mutex
{
public:
	mutex()             { pthread_mutex_init(&_mutex, NULL); }
	~mutex()            { pthread_mutex_destroy(&_mutex); }
	inline void lock()  { pthread_mutex_lock(&_mutex); }
	inline void unlock(){ pthread_mutex_unlock(&_mutex); }

	class scoped_lock
	{
	public:
		inline explicit scoped_lock(mutex & sp) : _sl(sp)  { _sl.lock(); }
		inline ~scoped_lock()                              { _sl.unlock(); }
	private:
		scoped_lock(scoped_lock const &);
		scoped_lock & operator=(scoped_lock const &);
		mutex&  _sl;
	};

private:
	pthread_mutex_t _mutex;
};
#endif


using namespace v8;
using namespace node;

namespace {

	struct OZW : ObjectWrap {
		static Handle<Value> New(const Arguments& args);
		static Handle<Value> Connect(const Arguments& args);
		static Handle<Value> Disconnect(const Arguments& args);
		static Handle<Value> SetValue(const Arguments& args);
		static Handle<Value> SetLocation(const Arguments& args);
		static Handle<Value> SetName(const Arguments& args);
		static Handle<Value> EnablePoll(const Arguments& args);
		static Handle<Value> DisablePoll(const Arguments& args);
		static Handle<Value> HardReset(const Arguments& args);
		static Handle<Value> SoftReset(const Arguments& args);
		static Handle<Value> SwitchAllOn(const Arguments& args);
		static Handle<Value> SwitchAllOff(const Arguments& args);
		static Handle<Value> CreateScene(const Arguments& args);
		static Handle<Value> RemoveScene(const Arguments& args);
		static Handle<Value> GetScenes(const Arguments& args);
		static Handle<Value> AddSceneValue(const Arguments& args);
		static Handle<Value> RemoveSceneValue(const Arguments& args);
		static Handle<Value> SceneGetValues(const Arguments& args);
		static Handle<Value> ActivateScene(const Arguments& args);
		static Handle<Value> HealNetworkNode(const Arguments& args);
		static Handle<Value> HealNetwork(const Arguments& args);
		static Handle<Value> GetNodeNeighbors(const Arguments& args);
		static Handle<Value> SetConfigParam(const Arguments& args);
	};

	Persistent<Object> context_obj;

	uv_async_t async;

	typedef struct {
		uint32_t type;
		uint32_t homeid;
		uint8_t nodeid;
		uint8_t groupidx;
		uint8_t event;
		uint8_t buttonid;
		uint8_t sceneid;
		uint8_t notification;
		std::list<OpenZWave::ValueID> values;
	} NotifInfo;

	typedef struct {
		uint32_t homeid;
		uint8_t nodeid;
		bool polled;
		std::list<OpenZWave::ValueID> values;
	} NodeInfo;

	typedef struct {
		uint32_t sceneid;
		std::string label;
		std::list<OpenZWave::ValueID> values;
	} SceneInfo;

	/*
	* Message passing queue between OpenZWave callback and v8 async handler.
	*/
	static mutex zqueue_mutex;
	static std::queue<NotifInfo *> zqueue;

	/*
	* Node state.
	*/
	static mutex znodes_mutex;
	static std::list<NodeInfo *> znodes;

	static mutex zscenes_mutex;

	static std::list<SceneInfo *> zscenes;

	static uint32_t homeid;

	Local<Object> zwaveValue2v8Value(OpenZWave::ValueID value);

	/*
	* Return the node for this request.
	*/
	NodeInfo *get_node_info(uint8_t nodeid) {
		std::list<NodeInfo *>::iterator it;

		NodeInfo *node;

		for (it = znodes.begin(); it != znodes.end(); ++it) {
			node = *it;
			if (node->nodeid == nodeid)
				return node;
		}

		return NULL;
	}

	SceneInfo *get_scene_info(uint8_t sceneid) {
		std::list<SceneInfo *>::iterator it;

		SceneInfo *scene;

		for (it = zscenes.begin(); it != zscenes.end(); ++it) {
			scene = *it;
			if (scene->sceneid == sceneid)
				return scene;
		}

		return NULL;
	}

	std::string* printAllArgs(const Arguments& args) {
		std::cout << "PRINTING ALL ARGS: ";

		std::string* stringArray = new std::string[args.Length()];

		for (int i = 0; i < args.Length(); i++){
			std::string tempString(*v8::String::Utf8Value(args[i]));
			stringArray[i] = tempString;
			std::cout << tempString << ";";
		}

		return stringArray;
	}

	/*
	* OpenZWave callback, just push onto queue and trigger the handler
	* in v8 land.
	*/
	void cb(OpenZWave::Notification const *cb, void *ctx) {
		NotifInfo *notif = new NotifInfo();

		notif->type = cb->GetType();
		notif->homeid = cb->GetHomeId();
		notif->nodeid = cb->GetNodeId();
		notif->values.push_front(cb->GetValueID());

		/*
		* Some values are only set on particular notifications, and
		* assertions in openzwave prevent us from trying to fetch them
		* unconditionally.
		*/
		switch (notif->type) {
		case OpenZWave::Notification::Type_Group:
			notif->groupidx = cb->GetGroupIdx();
			break;
		case OpenZWave::Notification::Type_NodeEvent:
			notif->event = cb->GetEvent();
			break;
		case OpenZWave::Notification::Type_CreateButton:
		case OpenZWave::Notification::Type_DeleteButton:
		case OpenZWave::Notification::Type_ButtonOn:
		case OpenZWave::Notification::Type_ButtonOff:
			notif->buttonid = cb->GetButtonId();
			break;
		case OpenZWave::Notification::Type_SceneEvent:
			notif->sceneid = cb->GetSceneId();
			break;
		case OpenZWave::Notification::Type_Notification:
			notif->notification = cb->GetNotification();
			break;
		}

		{
			mutex::scoped_lock sl(zqueue_mutex);
			zqueue.push(notif);
		}
		uv_async_send(&async);
	}

	/*
	* Async handler, triggered by the OpenZWave callback.
	*/
	void async_cb_handler(uv_async_t *handle, int status) {
		NodeInfo *node;
		NotifInfo *notif;
		Local < Value > args[16];


		mutex::scoped_lock sl(zqueue_mutex);

		while (!zqueue.empty()) {
			notif = zqueue.front();

			switch (notif->type) {
			case OpenZWave::Notification::Type_DriverReady:
				homeid = notif->homeid;
				args[0] = String::New("driver ready");
				args[1] = Integer::New(homeid);
				MakeCallback(context_obj, "emit", 2, args);
				break;
			case OpenZWave::Notification::Type_DriverFailed:
				args[0] = String::New("driver failed");
				MakeCallback(context_obj, "emit", 1, args);
				break;
				/*
				* NodeNew is triggered when a node is discovered which is not
				* found in the OpenZWave XML file.  As we do not use that file
				* simply ignore those notifications for now.
				*
				* NodeAdded is when we actually have a new node to set up.
				*/
			case OpenZWave::Notification::Type_NodeNew:
				break;
			case OpenZWave::Notification::Type_NodeAdded:
				node = new NodeInfo();
				node->homeid = notif->homeid;
				node->nodeid = notif->nodeid;
				node->polled = false;
				{
					mutex::scoped_lock sl(znodes_mutex);
					znodes.push_back(node);
				}
				args[0] = String::New("node added");
				args[1] = Integer::New(notif->nodeid);
				MakeCallback(context_obj, "emit", 2, args);
				break;
				/*
				* Ignore intermediate notifications about a node status, we
				* wait until the node is ready before retrieving information.
				*/
			case OpenZWave::Notification::Type_NodeProtocolInfo:
				break;
			case OpenZWave::Notification::Type_NodeNaming: {
				Local < Object > info = Object::New();
				info->Set(String::NewSymbol("manufacturer"), String::New(OpenZWave::Manager::Get()->GetNodeManufacturerName(notif->homeid, notif->nodeid).c_str()));
				info->Set(String::NewSymbol("manufacturerid"), String::New(OpenZWave::Manager::Get()->GetNodeManufacturerId(notif->homeid, notif->nodeid).c_str()));
				info->Set(String::NewSymbol("product"), String::New(OpenZWave::Manager::Get()->GetNodeProductName(notif->homeid, notif->nodeid).c_str()));
				info->Set(String::NewSymbol("producttype"), String::New(OpenZWave::Manager::Get()->GetNodeProductType(notif->homeid, notif->nodeid).c_str()));
				info->Set(String::NewSymbol("productid"), String::New(OpenZWave::Manager::Get()->GetNodeProductId(notif->homeid, notif->nodeid).c_str()));
				info->Set(String::NewSymbol("type"), String::New(OpenZWave::Manager::Get()->GetNodeType(notif->homeid, notif->nodeid).c_str()));
				info->Set(String::NewSymbol("name"), String::New(OpenZWave::Manager::Get()->GetNodeName(notif->homeid, notif->nodeid).c_str()));
				info->Set(String::NewSymbol("loc"), String::New(OpenZWave::Manager::Get()->GetNodeLocation(notif->homeid, notif->nodeid).c_str()));
				args[0] = String::New("node naming");
				args[1] = Integer::New(notif->nodeid);
				args[2] = info;
				MakeCallback(context_obj, "emit", 3, args);
				break;
			}
														   // XXX: these should be supported correctly.
			case OpenZWave::Notification::Type_PollingEnabled:
			case OpenZWave::Notification::Type_PollingDisabled:
				break;
				/*
				* Node values.
				*/
			case OpenZWave::Notification::Type_ValueAdded: {
				OpenZWave::ValueID value = notif->values.front();
				Local<Object> valobj = zwaveValue2v8Value(value);

				if ((node = get_node_info(notif->nodeid))) {
					mutex::scoped_lock sl(znodes_mutex);
					//pthread_mutex_lock(&znodes_mutex);
					node->values.push_back(value);
					//pthread_mutex_unlock(&znodes_mutex);
				}

				args[0] = String::New("value added");
				args[1] = Integer::New(notif->nodeid);
				args[2] = Integer::New(value.GetCommandClassId());
				args[3] = valobj;
				MakeCallback(context_obj, "emit", 4, args);
				break;
			}
			case OpenZWave::Notification::Type_ValueChanged: {
				OpenZWave::ValueID value = notif->values.front();
				Local<Object> valobj = zwaveValue2v8Value(value);

				args[0] = String::New("value changed");
				args[1] = Integer::New(notif->nodeid);
				args[2] = Integer::New(value.GetCommandClassId());
				args[3] = valobj;
				MakeCallback(context_obj, "emit", 4, args);
				break;
			}
			case OpenZWave::Notification::Type_ValueRefreshed: {
				OpenZWave::ValueID value = notif->values.front();
				Local<Object> valobj = zwaveValue2v8Value(value);

				args[0] = String::New("value refreshed");
				args[1] = Integer::New(notif->nodeid);
				args[2] = Integer::New(value.GetCommandClassId());
				args[3] = valobj;
				MakeCallback(context_obj, "emit", 4, args);
				break;
			}
			case OpenZWave::Notification::Type_ValueRemoved: {
				OpenZWave::ValueID value = notif->values.front();
				std::list<OpenZWave::ValueID>::iterator vit;
				if ((node = get_node_info(notif->nodeid))) {
					for (vit = node->values.begin(); vit != node->values.end(); ++vit) {
						if ((*vit) == notif->values.front()) {
							node->values.erase(vit);
							break;
						}
					}
				}
				args[0] = String::New("value removed");
				args[1] = Integer::New(notif->nodeid);
				args[2] = Integer::New(value.GetCommandClassId());
				args[3] = Integer::New(value.GetInstance());
				args[4] = Integer::New(value.GetIndex());
				MakeCallback(context_obj, "emit", 5, args);
				break;
			}
			
			/*
			 *Now node can accept commands.
			 */
			case OpenZWave::Notification::Type_EssentialNodeQueriesComplete: {
				Local < Object > info = Object::New();
				info->Set(String::NewSymbol("manufacturer"), String::New(OpenZWave::Manager::Get()->GetNodeManufacturerName(notif->homeid, notif->nodeid).c_str()));
				info->Set(String::NewSymbol("manufacturerid"), String::New(OpenZWave::Manager::Get()->GetNodeManufacturerId(notif->homeid, notif->nodeid).c_str()));
				info->Set(String::NewSymbol("product"), String::New(OpenZWave::Manager::Get()->GetNodeProductName(notif->homeid, notif->nodeid).c_str()));
				info->Set(String::NewSymbol("producttype"), String::New(OpenZWave::Manager::Get()->GetNodeProductType(notif->homeid, notif->nodeid).c_str()));
				info->Set(String::NewSymbol("productid"), String::New(OpenZWave::Manager::Get()->GetNodeProductId(notif->homeid, notif->nodeid).c_str()));
				info->Set(String::NewSymbol("type"), String::New(OpenZWave::Manager::Get()->GetNodeType(notif->homeid, notif->nodeid).c_str()));
				info->Set(String::NewSymbol("name"), String::New(OpenZWave::Manager::Get()->GetNodeName(notif->homeid, notif->nodeid).c_str()));
				info->Set(String::NewSymbol("loc"), String::New(OpenZWave::Manager::Get()->GetNodeLocation(notif->homeid, notif->nodeid).c_str()));
				args[0] = String::New("node available");
				args[1] = Integer::New(notif->nodeid);
				args[2] = info;
				MakeCallback(context_obj, "emit", 3, args);
				break;
			}
				/*
				* The node is now fully ready for operation.
				*/
			case OpenZWave::Notification::Type_NodeQueriesComplete: {
				Local < Object > info = Object::New();
				info->Set(String::NewSymbol("manufacturer"), String::New(OpenZWave::Manager::Get()->GetNodeManufacturerName(notif->homeid, notif->nodeid).c_str()));
				info->Set(String::NewSymbol("manufacturerid"), String::New(OpenZWave::Manager::Get()->GetNodeManufacturerId(notif->homeid, notif->nodeid).c_str()));
				info->Set(String::NewSymbol("product"), String::New(OpenZWave::Manager::Get()->GetNodeProductName(notif->homeid, notif->nodeid).c_str()));
				info->Set(String::NewSymbol("producttype"), String::New(OpenZWave::Manager::Get()->GetNodeProductType(notif->homeid, notif->nodeid).c_str()));
				info->Set(String::NewSymbol("productid"), String::New(OpenZWave::Manager::Get()->GetNodeProductId(notif->homeid, notif->nodeid).c_str()));
				info->Set(String::NewSymbol("type"), String::New(OpenZWave::Manager::Get()->GetNodeType(notif->homeid, notif->nodeid).c_str()));
				info->Set(String::NewSymbol("name"), String::New(OpenZWave::Manager::Get()->GetNodeName(notif->homeid, notif->nodeid).c_str()));
				info->Set(String::NewSymbol("loc"), String::New(OpenZWave::Manager::Get()->GetNodeLocation(notif->homeid, notif->nodeid).c_str()));
				args[0] = String::New("node ready");
				args[1] = Integer::New(notif->nodeid);
				args[2] = info;
				MakeCallback(context_obj, "emit", 3, args);
				break;
			}
																	/*
																	* The network scan has been completed.  Currently we do not
																	* care about dead nodes - is there anything we can do anyway?
																	*/
			case OpenZWave::Notification::Type_AwakeNodesQueried:
			case OpenZWave::Notification::Type_AllNodesQueried:
			case OpenZWave::Notification::Type_AllNodesQueriedSomeDead:
				args[0] = String::New("scan complete");
				MakeCallback(context_obj, "emit", 1, args);
				break;
			case OpenZWave::Notification::Type_NodeEvent: {
				args[0] = String::New("node event");
				args[1] = Integer::New(notif->nodeid);
				args[2] = Integer::New(notif->event);
				MakeCallback(context_obj, "emit", 3, args);
				break;
			}
			case OpenZWave::Notification::Type_SceneEvent:{
				args[0] = String::New("scene event");
				args[1] = Integer::New(notif->nodeid);
				args[2] = Integer::New(notif->sceneid);
				MakeCallback(context_obj, "emit", 3, args);
				break;
			}
														  /*
														  * A general notification.
														  */
			case OpenZWave::Notification::Type_Notification:
				args[0] = String::New("notification");
				args[1] = Integer::New(notif->nodeid);
				args[2] = Integer::New(notif->notification);
				MakeCallback(context_obj, "emit", 3, args);
				break;
			case OpenZWave::Notification::Type_Group:
				// Leave it for now
				break;
				/*
				* Send unhandled events to stderr so we can monitor them if
				* necessary.
				*/
			default:
				fprintf(stderr, "Unhandled notification: %d\n", notif->type);
				break;
			}

			zqueue.pop();
		}
	}

	Local<Object> zwaveValue2v8Value(OpenZWave::ValueID value) {
		Local <Object> valobj = Object::New();

		char buffer[15];

		sprintf(buffer, "%d-%d-%d-%d", value.GetNodeId(), value.GetCommandClassId(), value.GetInstance(), value.GetIndex());

		valobj->Set(String::NewSymbol("value_id"), String::New(buffer));

		/*
		* Common value types.
		*/
		valobj->Set(String::NewSymbol("id"), Integer::New(value.GetId()));
		valobj->Set(String::NewSymbol("node_id"), Integer::New(value.GetNodeId()));
		valobj->Set(String::NewSymbol("class_id"), Integer::New(value.GetCommandClassId()));
		valobj->Set(String::NewSymbol("type"), String::New(OpenZWave::Value::GetTypeNameFromEnum(value.GetType())));
		valobj->Set(String::NewSymbol("genre"), String::New(OpenZWave::Value::GetGenreNameFromEnum(value.GetGenre())));
		valobj->Set(String::NewSymbol("instance"), Integer::New(value.GetInstance()));
		valobj->Set(String::NewSymbol("index"), Integer::New(value.GetIndex()));
		valobj->Set(String::NewSymbol("label"), String::New(OpenZWave::Manager::Get()->GetValueLabel(value).c_str()));
		valobj->Set(String::NewSymbol("units"), String::New(OpenZWave::Manager::Get()->GetValueUnits(value).c_str()));
		valobj->Set(String::NewSymbol("read_only"), Boolean::New(OpenZWave::Manager::Get()->IsValueReadOnly(value))->ToBoolean());
		valobj->Set(String::NewSymbol("write_only"), Boolean::New(OpenZWave::Manager::Get()->IsValueWriteOnly(value))->ToBoolean());
		// XXX: verify_changes=
		// XXX: poll_intensity=
		valobj->Set(String::NewSymbol("min"), Integer::New(OpenZWave::Manager::Get()->GetValueMin(value)));
		valobj->Set(String::NewSymbol("max"), Integer::New(OpenZWave::Manager::Get()->GetValueMax(value)));

		/*
		* The value itself is type-specific.
		*/
		switch (value.GetType()) {
		case OpenZWave::ValueID::ValueType_Bool: {
			bool val;
			OpenZWave::Manager::Get()->GetValueAsBool(value, &val);
			valobj->Set(String::NewSymbol("value"), Boolean::New(val)->ToBoolean());
			break;
		}
		case OpenZWave::ValueID::ValueType_Byte: {
			uint8_t val;
			OpenZWave::Manager::Get()->GetValueAsByte(value, &val);
			valobj->Set(String::NewSymbol("value"), Integer::New(val));
			break;
		}
		case OpenZWave::ValueID::ValueType_Decimal: {
			float val;
			OpenZWave::Manager::Get()->GetValueAsFloat(value, &val);
			valobj->Set(String::NewSymbol("value"), Integer::New(val));
			break;
		}
		case OpenZWave::ValueID::ValueType_Int: {
			int32_t val;
			OpenZWave::Manager::Get()->GetValueAsInt(value, &val);
			valobj->Set(String::NewSymbol("value"), Integer::New(val));
			break;
		}
		case OpenZWave::ValueID::ValueType_List: {
			std::vector < std::string > items;
			OpenZWave::Manager::Get()->GetValueListItems(value, &items);
			Local < Array > values = Array::New(items.size());
			for (unsigned i = 0; i < items.size(); i++) {
				values->Set(Number::New(i), String::New(&items[i][0], items[i].size()));
			}
			valobj->Set(String::NewSymbol("values"), values);
			std::string val;
			OpenZWave::Manager::Get()->GetValueListSelection(value, &val);
			valobj->Set(String::NewSymbol("value"), String::New(val.c_str()));
			break;
		}
		case OpenZWave::ValueID::ValueType_Short: {
			int16_t val;
			OpenZWave::Manager::Get()->GetValueAsShort(value, &val);
			valobj->Set(String::NewSymbol("value"), Integer::New(val));
			break;
		}
		case OpenZWave::ValueID::ValueType_String: {
			std::string val;
			OpenZWave::Manager::Get()->GetValueAsString(value, &val);
			valobj->Set(String::NewSymbol("value"), String::New(val.c_str()));
			break;
		}
												   /*
												   * Buttons do not have a value.
												   */
		case OpenZWave::ValueID::ValueType_Button: {
			break;
		}
		case OpenZWave::ValueID::ValueType_Schedule: {
			break;
		}
		case OpenZWave::ValueID::ValueType_Raw: {
			break;
		}
		default: {
			fprintf(stderr, "unsupported value type: 0x%x\n", value.GetType());
			break;
		}
		}

		return valobj;
	}

	Local<Object> zwaveSceneValue2v8Value(uint8 sceneId, OpenZWave::ValueID value) {
		Local <Object> valobj = Object::New();

		char buffer[15];

		sprintf(buffer, "%d-%d-%d-%d", value.GetNodeId(), value.GetCommandClassId(), value.GetInstance(), value.GetIndex());

		valobj->Set(String::NewSymbol("value_id"), String::New(buffer));

		/*
		* Common value types.
		*/
		valobj->Set(String::NewSymbol("id"), Integer::New(value.GetId()));
		valobj->Set(String::NewSymbol("node_id"), Integer::New(value.GetNodeId()));
		valobj->Set(String::NewSymbol("class_id"), Integer::New(value.GetCommandClassId()));
		valobj->Set(String::NewSymbol("type"), String::New(OpenZWave::Value::GetTypeNameFromEnum(value.GetType())));
		valobj->Set(String::NewSymbol("genre"), String::New(OpenZWave::Value::GetGenreNameFromEnum(value.GetGenre())));
		valobj->Set(String::NewSymbol("instance"), Integer::New(value.GetInstance()));
		valobj->Set(String::NewSymbol("index"), Integer::New(value.GetIndex()));
		valobj->Set(String::NewSymbol("label"), String::New(OpenZWave::Manager::Get()->GetValueLabel(value).c_str()));
		valobj->Set(String::NewSymbol("units"), String::New(OpenZWave::Manager::Get()->GetValueUnits(value).c_str()));
		valobj->Set(String::NewSymbol("read_only"), Boolean::New(OpenZWave::Manager::Get()->IsValueReadOnly(value))->ToBoolean());
		valobj->Set(String::NewSymbol("write_only"), Boolean::New(OpenZWave::Manager::Get()->IsValueWriteOnly(value))->ToBoolean());
		// XXX: verify_changes=
		// XXX: poll_intensity=
		valobj->Set(String::NewSymbol("min"), Integer::New(OpenZWave::Manager::Get()->GetValueMin(value)));
		valobj->Set(String::NewSymbol("max"), Integer::New(OpenZWave::Manager::Get()->GetValueMax(value)));

		/*
		* The value itself is type-specific.
		*/
		switch (value.GetType()) {
		case OpenZWave::ValueID::ValueType_Bool: {
			bool val;
			OpenZWave::Manager::Get()->SceneGetValueAsBool(sceneId, value, &val);
			valobj->Set(String::NewSymbol("value"), Boolean::New(val)->ToBoolean());
			break;
		}
		case OpenZWave::ValueID::ValueType_Byte: {
			uint8_t val;
			OpenZWave::Manager::Get()->SceneGetValueAsByte(sceneId, value, &val);
			valobj->Set(String::NewSymbol("value"), Integer::New(val));
			break;
		}
		case OpenZWave::ValueID::ValueType_Decimal: {
			float val;
			OpenZWave::Manager::Get()->SceneGetValueAsFloat(sceneId, value, &val);
			valobj->Set(String::NewSymbol("value"), Integer::New(val));
			break;
		}
		case OpenZWave::ValueID::ValueType_Int: {
			int32_t val;
			OpenZWave::Manager::Get()->SceneGetValueAsInt(sceneId, value, &val);
			valobj->Set(String::NewSymbol("value"), Integer::New(val));
			break;
		}
		case OpenZWave::ValueID::ValueType_List: {
			std::vector < std::string > items;
			OpenZWave::Manager::Get()->GetValueListItems(value, &items);
			Local < Array > values = Array::New(items.size());
			for (unsigned i = 0; i < items.size(); i++) {
				values->Set(Number::New(i), String::New(&items[i][0], items[i].size()));
			}
			valobj->Set(String::NewSymbol("values"), values);
			std::string val;
			OpenZWave::Manager::Get()->SceneGetValueListSelection(sceneId, value, &val);
			valobj->Set(String::NewSymbol("value"), String::New(val.c_str()));
			break;
		}
		case OpenZWave::ValueID::ValueType_Short: {
			int16_t val;
			OpenZWave::Manager::Get()->SceneGetValueAsShort(sceneId, value, &val);
			valobj->Set(String::NewSymbol("value"), Integer::New(val));
			break;
		}
		case OpenZWave::ValueID::ValueType_String: {
			std::string val;
			OpenZWave::Manager::Get()->SceneGetValueAsString(sceneId, value, &val);
			valobj->Set(String::NewSymbol("value"), String::New(val.c_str()));
			break;
		}
		default: {
			fprintf(stderr, "unsupported scene value type: 0x%x\n", value.GetType());
			break;
		}
		}

		return valobj;
	}

	Handle<Value> OZW::New(const Arguments& args) {
		HandleScope scope;

		assert(args.IsConstructCall());
		OZW* self = new OZW();
		self->Wrap(args.This());

		Local < Object > opts = args[0]->ToObject();
		std::string confpath = (*String::Utf8Value(opts->Get(String::New("modpath")->ToString())));
		confpath += "/../deps/open-zwave/config";

		/*
		* Options are global for all drivers and can only be set once.
		*/
		OpenZWave::Options::Create(confpath.c_str(), "", "");
		OpenZWave::Options::Get()->AddOptionBool("ConsoleOutput", opts->Get(String::New("consoleoutput"))->BooleanValue());
		OpenZWave::Options::Get()->AddOptionBool("Logging", opts->Get(String::New("logging"))->BooleanValue());
		OpenZWave::Options::Get()->AddOptionBool("SaveConfiguration", opts->Get(String::New("saveconfig"))->BooleanValue());
		OpenZWave::Options::Get()->AddOptionInt("DriverMaxAttempts", opts->Get(String::New("driverattempts"))->IntegerValue());
		OpenZWave::Options::Get()->AddOptionInt("PollInterval", opts->Get(String::New("pollinterval"))->IntegerValue());
		OpenZWave::Options::Get()->AddOptionBool("IntervalBetweenPolls", true);
		OpenZWave::Options::Get()->AddOptionBool("SuppressValueRefresh", opts->Get(String::New("suppressrefresh"))->BooleanValue());
		OpenZWave::Options::Get()->Lock();

		return scope.Close(args.This());
	}

	Handle<Value> OZW::Connect(const Arguments& args) {
		HandleScope scope;

		std::string path = (*String::Utf8Value(args[0]->ToString()));

		uv_async_init(uv_default_loop(), &async, async_cb_handler);

		context_obj = Persistent < Object > ::New(args.This());

		OpenZWave::Manager::Create();
		OpenZWave::Manager::Get()->AddWatcher(cb, NULL);
		OpenZWave::Manager::Get()->AddDriver(path);

		Handle<Value> argv[1] = { String::New("connected") };
		MakeCallback(context_obj, "emit", 1, argv);

		return Undefined();
	}

	Handle<Value> OZW::Disconnect(const Arguments& args) {
		HandleScope scope;

		std::string path = (*String::Utf8Value(args[0]->ToString()));

		OpenZWave::Manager::Get()->RemoveDriver(path);
		OpenZWave::Manager::Get()->RemoveWatcher(cb, NULL);
		OpenZWave::Manager::Destroy();
		OpenZWave::Options::Destroy();

		return scope.Close(Undefined());
	}

	/*
	* Generic value set.
	*/
	Handle<Value> OZW::SetValue(const Arguments& args) {
		HandleScope scope;

		uint8_t nodeid = args[0]->ToNumber()->Value();
		uint8_t comclass = args[1]->ToNumber()->Value();
		uint8_t instance = args[2]->ToNumber()->Value();
		uint8_t index = args[3]->ToNumber()->Value();

		NodeInfo *node;
		std::list<OpenZWave::ValueID>::iterator vit;

		if ((node = get_node_info(nodeid))) {
			for (vit = node->values.begin(); vit != node->values.end(); ++vit) {
				if (((*vit).GetCommandClassId() == comclass) && ((*vit).GetInstance() == instance) && ((*vit).GetIndex() == index)) {

					switch ((*vit).GetType()) {
					case OpenZWave::ValueID::ValueType_Bool: {
						bool val = args[4]->ToBoolean()->Value();
						OpenZWave::Manager::Get()->SetValue(*vit, val);
						break;
					}
					case OpenZWave::ValueID::ValueType_Byte: {
						uint8_t val = args[4]->ToInteger()->Value();
						OpenZWave::Manager::Get()->SetValue(*vit, val);
						break;
					}
					case OpenZWave::ValueID::ValueType_Decimal: {
						float val = args[4]->ToNumber()->NumberValue();
						OpenZWave::Manager::Get()->SetValue(*vit, val);
						break;
					}
					case OpenZWave::ValueID::ValueType_Int: {
						int32_t val = args[4]->ToInteger()->Value();
						OpenZWave::Manager::Get()->SetValue(*vit, val);
						break;
					}
					case OpenZWave::ValueID::ValueType_List: {
						std::string val = (*String::Utf8Value(args[4]->ToString()));
						OpenZWave::Manager::Get()->SetValue(*vit, val);
						break;
					}
					case OpenZWave::ValueID::ValueType_Short: {
						int16_t val = args[4]->ToInteger()->Value();
						OpenZWave::Manager::Get()->SetValue(*vit, val);
						break;
					}
					case OpenZWave::ValueID::ValueType_String: {
						std::string val = (*String::Utf8Value(args[4]->ToString()));
						OpenZWave::Manager::Get()->SetValue(*vit, val);
						break;
					}
					case OpenZWave::ValueID::ValueType_Schedule: {
						break;
					}
					case OpenZWave::ValueID::ValueType_Button: {
						break;
					}
					}
				}
			}
		}

		return scope.Close(Undefined());
	}

	/*
	* Write a new location string to the device, if supported.
	*/
	Handle<Value> OZW::SetLocation(const Arguments& args) {
		HandleScope scope;

		uint8_t nodeid = args[0]->ToNumber()->Value();
		std::string location = (*String::Utf8Value(args[1]->ToString()));

		OpenZWave::Manager::Get()->SetNodeLocation(homeid, nodeid, location);

		return scope.Close(Undefined());
	}

	/*
	* Write a new name string to the device, if supported.
	*/
	Handle<Value> OZW::SetName(const Arguments& args) {
		HandleScope scope;

		uint8_t nodeid = args[0]->ToNumber()->Value();
		std::string name = (*String::Utf8Value(args[1]->ToString()));

		OpenZWave::Manager::Get()->SetNodeName(homeid, nodeid, name);

		return scope.Close(Undefined());
	}

	/*
	* Enable/Disable polling on a COMMAND_CLASS basis.
	*/
	Handle<Value> OZW::EnablePoll(const Arguments& args) {
		HandleScope scope;

		uint8_t nodeid = args[0]->ToNumber()->Value();
		uint8_t comclass = args[1]->ToNumber()->Value();
		NodeInfo *node;
		std::list<OpenZWave::ValueID>::iterator vit;

		if ((node = get_node_info(nodeid))) {
			for (vit = node->values.begin(); vit != node->values.end(); ++vit) {
				if ((*vit).GetCommandClassId() == comclass) {
					OpenZWave::Manager::Get()->EnablePoll((*vit), 1);
					break;
				}
			}
		}

		return scope.Close(Undefined());
	}

	Handle<Value> OZW::DisablePoll(const Arguments& args) {
		HandleScope scope;

		uint8_t nodeid = args[0]->ToNumber()->Value();
		uint8_t comclass = args[1]->ToNumber()->Value();
		NodeInfo *node;
		std::list<OpenZWave::ValueID>::iterator vit;

		if ((node = get_node_info(nodeid))) {
			for (vit = node->values.begin(); vit != node->values.end(); ++vit) {
				if ((*vit).GetCommandClassId() == comclass) {
					OpenZWave::Manager::Get()->DisablePoll((*vit));
					break;
				}
			}
		}

		return scope.Close(Undefined());
	}

	/*
	* Reset the ZWave controller chip.  A hard reset is destructive and wipes
	* out all known configuration, a soft reset just restarts the chip.
	*/
	Handle<Value> OZW::HardReset(const Arguments& args) {
		HandleScope scope;

		OpenZWave::Manager::Get()->ResetController(homeid);

		return scope.Close(Undefined());
	}

	Handle<Value> OZW::SoftReset(const Arguments& args) {
		HandleScope scope;

		OpenZWave::Manager::Get()->SoftReset(homeid);

		return scope.Close(Undefined());
	}

	Handle<Value> OZW::SwitchAllOn(const Arguments& args) {
		HandleScope scope;

		OpenZWave::Manager::Get()->SwitchAllOn(homeid);

		return scope.Close(Undefined());
	}

	Handle<Value> OZW::SwitchAllOff(const Arguments& args) {
		HandleScope scope;

		OpenZWave::Manager::Get()->SwitchAllOff(homeid);

		return scope.Close(Undefined());
	}

	Handle<Value> OZW::CreateScene(const Arguments& args) {
		HandleScope scope;

		std::string label = (*String::Utf8Value(args[0]->ToString()));

		SceneInfo *scene;

		uint8 sceneid = OpenZWave::Manager::Get()->CreateScene();

		if (sceneid > 0) {
			OpenZWave::Manager::Get()->SetSceneLabel(sceneid, label);
			scene = new SceneInfo();
			scene->sceneid = sceneid;
			scene->label = label;
			mutex::scoped_lock sl(zscenes_mutex);
			zscenes.push_back(scene);
		}

		return scope.Close(Undefined());
	}

	Handle<Value> OZW::RemoveScene(const Arguments& args) {
		HandleScope scope;

		uint8_t sceneid = args[0]->ToNumber()->Value();

		SceneInfo *scene;

		if ((scene = get_scene_info(sceneid))) {
			OpenZWave::Manager::Get()->RemoveScene(sceneid);
			mutex::scoped_lock sl(zscenes_mutex);
			zscenes.remove(scene);
		}

		return scope.Close(Undefined());
	}

	Handle<Value> OZW::GetScenes(const Arguments& args) {
		HandleScope scope;

		uint8_t numscenes = OpenZWave::Manager::Get()->GetNumScenes();
		Local <Value> cbargs[16];
		SceneInfo *scene;

		if (numscenes != zscenes.size()) {
			{
				mutex::scoped_lock sl(zscenes_mutex);
				zscenes.clear();
			}
			uint8_t *sceneids;
			sceneids = new uint8_t[numscenes];

			OpenZWave::Manager::Get()->GetAllScenes(&sceneids);

			for (unsigned i = 0; i < numscenes; i++) {
				scene = new SceneInfo();
				scene->sceneid = sceneids[i];

				scene->label = OpenZWave::Manager::Get()->GetSceneLabel(sceneids[i]);
				mutex::scoped_lock sl(zscenes_mutex);
				zscenes.push_back(scene);
			}
		}

		Local<Array> scenes = Array::New(zscenes.size());
		std::list<SceneInfo *>::iterator it;
		unsigned j = 0;

		for (it = zscenes.begin(); it != zscenes.end(); ++it) {
			scene = *it;

			Local <Object> info = Object::New();
			info->Set(String::NewSymbol("sceneid"), Integer::New(scene->sceneid));
			info->Set(String::NewSymbol("label"), String::New(scene->label.c_str()));

			scenes->Set(Number::New(j++), info);
		}

		cbargs[0] = String::New("scenes list");
		cbargs[1] = scenes;

		MakeCallback(context_obj, "emit", 2, cbargs);

		return scope.Close(Undefined());
	}

	Handle<Value> OZW::AddSceneValue(const Arguments& args) {
		HandleScope scope;

		uint8_t sceneid = args[0]->ToNumber()->Value();
		uint8_t nodeid = args[1]->ToNumber()->Value();
		uint8_t comclass = args[2]->ToNumber()->Value();
		uint8_t instance = args[3]->ToNumber()->Value();
		uint8_t index = args[4]->ToNumber()->Value();

		NodeInfo *node;
		std::list<OpenZWave::ValueID>::iterator vit;

		if ((node = get_node_info(nodeid))) {
			for (vit = node->values.begin(); vit != node->values.end(); ++vit) {
				if (((*vit).GetCommandClassId() == comclass) && ((*vit).GetInstance() == instance) && ((*vit).GetIndex() == index)) {

					switch ((*vit).GetType()) {
					case OpenZWave::ValueID::ValueType_Bool: {
						//bool val; OpenZWave::Manager::Get()->GetValueAsBool(*vit, &val);
						bool val = args[5]->ToBoolean()->Value();
						OpenZWave::Manager::Get()->AddSceneValue(sceneid, *vit, val);
						break;
					}
					case OpenZWave::ValueID::ValueType_Byte: {
						//uint8_t val; OpenZWave::Manager::Get()->GetValueAsByte(*vit, &val);
						uint8_t val = args[5]->ToInteger()->Value();
						OpenZWave::Manager::Get()->AddSceneValue(sceneid, *vit, val);
						break;
					}
					case OpenZWave::ValueID::ValueType_Decimal: {
						//float val; OpenZWave::Manager::Get()->GetValueAsFloat(*vit, &val);
						float val = args[5]->ToNumber()->NumberValue();
						OpenZWave::Manager::Get()->AddSceneValue(sceneid, *vit, val);
						break;
					}
					case OpenZWave::ValueID::ValueType_Int: {
						//int32_t val; OpenZWave::Manager::Get()->GetValueAsInt(*vit, &val);
						int32_t val = args[5]->ToInteger()->Value();
						OpenZWave::Manager::Get()->AddSceneValue(sceneid, *vit, val);
						break;
					}
					case OpenZWave::ValueID::ValueType_List: {
						//std::string val; OpenZWave::Manager::Get()->GetValueListSelection(*vit, &val);
						std::string val = (*String::Utf8Value(args[5]->ToString()));
						OpenZWave::Manager::Get()->AddSceneValue(sceneid, *vit, val);
						break;
					}
					case OpenZWave::ValueID::ValueType_Short: {
						//int16_t val; OpenZWave::Manager::Get()->GetValueAsShort(*vit, &val);
						int16_t val = args[5]->ToInteger()->Value();
						OpenZWave::Manager::Get()->AddSceneValue(sceneid, *vit, val);
						break;
					}
					case OpenZWave::ValueID::ValueType_String: {
						//std::string val; OpenZWave::Manager::Get()->GetValueAsString(*vit, &val);
						std::string val = (*String::Utf8Value(args[5]->ToString()));
						OpenZWave::Manager::Get()->AddSceneValue(sceneid, *vit, val);
						break;
					}
					case OpenZWave::ValueID::ValueType_Schedule: {
						break;
					}
					case OpenZWave::ValueID::ValueType_Button: {
						break;
					}
					}
				}
			}
		}

		return scope.Close(Undefined());
	}

	Handle<Value> OZW::RemoveSceneValue(const Arguments& args) {
		HandleScope scope;

		uint8_t sceneid = args[0]->ToNumber()->Value();
		uint8_t nodeid = args[1]->ToNumber()->Value();
		uint8_t comclass = args[2]->ToNumber()->Value();
		uint8_t instance = args[3]->ToNumber()->Value();
		uint8_t index = args[4]->ToNumber()->Value();

		SceneInfo *scene;

		std::list<OpenZWave::ValueID>::iterator vit;

		if ((scene = get_scene_info(sceneid))) {
			for (vit = scene->values.begin(); vit != scene->values.end(); ++vit) {
				if (((*vit).GetNodeId() == nodeid) && ((*vit).GetCommandClassId() == comclass) && ((*vit).GetInstance() == instance) && ((*vit).GetIndex() == index)) {
					OpenZWave::Manager::Get()->RemoveSceneValue(sceneid, *vit);
					scene->values.erase(vit);
					break;
				}
			}
		}

		return scope.Close(Undefined());
	}

	Handle<Value> OZW::SceneGetValues(const Arguments& args) {
		HandleScope scope;

		uint8_t sceneid = args[0]->ToNumber()->Value();

		std::vector<OpenZWave::ValueID> values;

		std::vector<OpenZWave::ValueID>::iterator vit;

		OpenZWave::Manager::Get()->SceneGetValues(sceneid, &values);

		SceneInfo *scene;

		if ((scene = get_scene_info(sceneid))) {
			scene->values.clear();

			Local <Value> cbargs[16];

			Local<Array> v8values = Array::New(scene->values.size());

			unsigned j = 0;

			for (vit = values.begin(); vit != values.end(); ++vit) {
				mutex::scoped_lock sl(zscenes_mutex);
				scene->values.push_back(*vit);

				v8values->Set(Number::New(j++), zwaveSceneValue2v8Value(sceneid, *vit));
			}

			cbargs[0] = String::New("scene values list");
			cbargs[1] = v8values;

			MakeCallback(context_obj, "emit", 2, cbargs);
		}

		return scope.Close(Undefined());
	}

	Handle<Value> OZW::ActivateScene(const Arguments& args) {
		HandleScope scope;

		uint8_t sceneid = args[0]->ToNumber()->Value();

		SceneInfo *scene;

		if ((scene = get_scene_info(sceneid))) {
			OpenZWave::Manager::Get()->ActivateScene(sceneid);
		}

		return scope.Close(Undefined());
	}

	/*
	* Heal network node by requesting the node rediscover their neighbors.
	*/
	Handle<Value> OZW::HealNetworkNode(const Arguments& args)
	{
		HandleScope scope;

		uint8_t nodeid = args[0]->ToNumber()->Value();
		uint8_t doRR = args[1]->ToBoolean()->Value();

		OpenZWave::Manager::Get()->HealNetworkNode(homeid, nodeid, doRR);

		return scope.Close(Undefined());
	}

	/*
	* Heal network by requesting node's rediscover their neighbors.
	* Sends a ControllerCommand_RequestNodeNeighborUpdate to every node.
	* Can take a while on larger networks.
	*/
	Handle<Value> OZW::HealNetwork(const Arguments& args)
	{
		HandleScope scope;

		bool doRR = true;
		OpenZWave::Manager::Get()->HealNetwork(homeid, doRR);

		return scope.Close(Undefined());
	}

	/*
	* Gets the neighbors for a node
	*/
	Handle<Value> OZW::GetNodeNeighbors(const Arguments& args)
	{
		HandleScope scope;
		uint8* neighbors;

		uint8_t nodeid = args[0]->ToNumber()->Value();
		uint8 numNeighbors = OpenZWave::Manager::Get()->GetNodeNeighbors(homeid, nodeid, &neighbors);
		Local<Array> o_neighbors = Array::New(numNeighbors);

		for (uint8 nr = 0; nr < numNeighbors; nr++) {
			o_neighbors->Set(Integer::New(nr), Integer::New(neighbors[nr]));
		}

		Local<Value> argv[3];
		argv[0] = String::New("neighbors");
		argv[1] = Integer::New(nodeid);
		argv[2] = o_neighbors;

		MakeCallback(context_obj, "emit", 3, argv);

		return scope.Close(Undefined());
	}

	/*
	* Set Config Parameters
	*/
	Handle<Value> OZW::SetConfigParam(const Arguments& args)
	{
		HandleScope scope;

		uint32_t homeid = args[0]->ToNumber()->Value();
		uint8_t nodeid = args[1]->ToNumber()->Value();
		uint8_t param = args[2]->ToNumber()->Value();
		int32_t value = args[3]->ToNumber()->Value();

		if (args.Length() < 5) {
			OpenZWave::Manager::Get()->SetConfigParam(homeid, nodeid, param, value);
		}
		else {
			uint8_t size = args[4]->ToNumber()->Value();
			OpenZWave::Manager::Get()->SetConfigParam(homeid, nodeid, param, value, size);
		}

		return scope.Close(Undefined());
	}

	extern "C" void init(Handle<Object> target) {
		HandleScope scope;

		Local < FunctionTemplate > t = FunctionTemplate::New(OZW::New);
		t->InstanceTemplate()->SetInternalFieldCount(1);
		t->SetClassName(String::New("OZW"));

		NODE_SET_PROTOTYPE_METHOD(t, "connect", OZW::Connect);
		NODE_SET_PROTOTYPE_METHOD(t, "disconnect", OZW::Disconnect);
		NODE_SET_PROTOTYPE_METHOD(t, "setValue", OZW::SetValue);
		NODE_SET_PROTOTYPE_METHOD(t, "setLocation", OZW::SetLocation);
		NODE_SET_PROTOTYPE_METHOD(t, "setName", OZW::SetName);
		NODE_SET_PROTOTYPE_METHOD(t, "enablePoll", OZW::EnablePoll);
		NODE_SET_PROTOTYPE_METHOD(t, "disablePoll", OZW::EnablePoll);
		NODE_SET_PROTOTYPE_METHOD(t, "hardReset", OZW::HardReset);
		NODE_SET_PROTOTYPE_METHOD(t, "softReset", OZW::SoftReset);
		NODE_SET_PROTOTYPE_METHOD(t, "allOn", OZW::SwitchAllOn);
		NODE_SET_PROTOTYPE_METHOD(t, "allOff", OZW::SwitchAllOff);
		NODE_SET_PROTOTYPE_METHOD(t, "createScene", OZW::CreateScene);
		NODE_SET_PROTOTYPE_METHOD(t, "removeScene", OZW::RemoveScene);
		NODE_SET_PROTOTYPE_METHOD(t, "getScenes", OZW::GetScenes);
		NODE_SET_PROTOTYPE_METHOD(t, "addSceneValue", OZW::AddSceneValue);
		NODE_SET_PROTOTYPE_METHOD(t, "removeSceneValue", OZW::RemoveSceneValue);
		NODE_SET_PROTOTYPE_METHOD(t, "sceneGetValues", OZW::SceneGetValues);
		NODE_SET_PROTOTYPE_METHOD(t, "activateScene", OZW::ActivateScene);
		NODE_SET_PROTOTYPE_METHOD(t, "healNetworkNode", OZW::HealNetworkNode);
		NODE_SET_PROTOTYPE_METHOD(t, "healNetwork", OZW::HealNetwork);
		NODE_SET_PROTOTYPE_METHOD(t, "getNeighbors", OZW::GetNodeNeighbors);
		NODE_SET_PROTOTYPE_METHOD(t, "setConfigParam", OZW::SetConfigParam);

		target->Set(String::NewSymbol("Emitter"), t->GetFunction());
	}

}

NODE_MODULE(openzwave, init)