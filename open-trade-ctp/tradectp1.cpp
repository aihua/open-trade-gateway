/////////////////////////////////////////////////////////////////////////
///@file tradectp1.cpp
///@brief	CTP交易逻辑实现
///@copyright	上海信易信息科技股份有限公司 版权所有
/////////////////////////////////////////////////////////////////////////

#include "tradectp.h"
#include "utility.h"
#include "config.h"
#include "ins_list.h"
#include "numset.h"
#include "SerializerTradeBase.h"

#include <functional>

static std::string GuessExchangeId(std::string instrument_id)
{
	if (instrument_id.size() > 11) 
	{
		//组合
		if ((instrument_id[0] == 'S' && instrument_id[1] == 'P' && instrument_id[2] == 'D')
			|| (instrument_id[0] == 'I' && instrument_id[1] == 'P' && instrument_id[2] == 'S')
			)
			return "CZCE";
		else
			return "DCE";
	}
	if (instrument_id.size() > 8
		&& instrument_id[0] == 'm'
		&& instrument_id[5] == '-'
		&& (instrument_id[6] == 'C' || instrument_id[6] == 'P')
		&& instrument_id[7] == '-'
		) 
	{
		//大连期权
		//"^DCE\.m(\d\d)(\d\d)-([CP])-(\d+)$"
		return "DCE";
	}
	if (instrument_id.size() > 7
		&& instrument_id[0] == 'c'
		&& (instrument_id[6] == 'C' || instrument_id[6] == 'P')
		) {
		//上海期权
		//"^SHFE\.cu(\d\d)(\d\d)([CP])(\d+)$"
		return "SHFE";
	}
	if (instrument_id.size() > 6
		&& instrument_id[0] == 'S'
		&& instrument_id[1] == 'R'
		&& (instrument_id[5] == 'C' || instrument_id[5] == 'P')
		) {
		//郑州期权
		//"CZCE\.SR(\d)(\d\d)([CP])(\d+)"
		return "CZCE";
	}
	if (instrument_id.size() == 5
		&& instrument_id[0] >= 'A' && instrument_id[0] <= 'Z'
		&& instrument_id[1] >= 'A' && instrument_id[1] <= 'Z'
		) {
		//郑州期货
		return "CZCE";
	}
	if (instrument_id.size() == 5
		&& instrument_id[0] >= 'a' && instrument_id[0] <= 'z'
		&& instrument_id[1] >= '0' && instrument_id[1] <= '9'
		) {
		//大连期货
		return "DCE";
	}
	if (instrument_id.size() == 5
		&& instrument_id[0] >= 'A' && instrument_id[0] <= 'Z'
		) {
		//中金期货
		return "CFFEX";
	}
	if (instrument_id.size() == 6
		&& instrument_id[0] >= 'A' && instrument_id[0] <= 'Z'
		&& instrument_id[1] >= 'A' && instrument_id[1] <= 'Z'
		) {
		//中金期货
		return "CFFEX";
	}
	if (instrument_id.size() == 6
		&& instrument_id[0] >= 'a' && instrument_id[0] <= 'z'
		&& instrument_id[1] >= 'a' && instrument_id[1] <= 'z'
		) {
		if ((instrument_id[0] == 'c' && instrument_id[1] == 's')
			|| (instrument_id[0] == 'f' && instrument_id[1] == 'b')
			|| (instrument_id[0] == 'b' && instrument_id[1] == 'b')
			|| (instrument_id[0] == 'j' && instrument_id[1] == 'd')
			|| (instrument_id[0] == 'p' && instrument_id[1] == 'p')
			|| (instrument_id[0] == 'e' && instrument_id[1] == 'g')
			|| (instrument_id[0] == 'j' && instrument_id[1] == 'm'))
			return "DCE";
		if (instrument_id[0] == 's' && instrument_id[1] == 'c')
			return "INE";
		return "SHFE";
	}
	return "UNKNOWN";
}

void traderctp::ProcessOnFrontConnected()
{
	ReqAuthenticate();
	Log(LOG_INFO, NULL
		, "ctp ProcessOnFrontConnected, instance=%p, UserID=%s"
		, this, _req_login.user_name.c_str());
	OutputNotifyAllSycn(0,u8"已经重新连接到交易前置");
}

void traderctp::OnFrontConnected()
{
	//还在等待登录阶段
	if (!m_b_login.load())
	{
		//这时是安全的
		ReqAuthenticate();
		Log(LOG_INFO, NULL, "ctp OnFrontConnected, instance=%p, UserID=%s"
			, this, _req_login.user_name.c_str());
		OutputNotifySycn(m_loging_connectId,0,u8"已经连接到交易前置");
	}
	else
	{
		//这时不能直接调用
		_ios.post(boost::bind(&traderctp::ProcessOnFrontConnected
			, this));
	}	
}

void traderctp::ReqAuthenticate()
{
	if (_req_login.broker.auth_code.empty())
	{
		Log(LOG_INFO, NULL, "_req_login.broker.auth_code.empty(), instance=%p, UserID=%s"
			, this, _req_login.user_name.c_str());
		SendLoginRequest();
		return;
	}
	CThostFtdcReqAuthenticateField field;
	memset(&field, 0, sizeof(field));
	strcpy_x(field.BrokerID, m_broker_id.c_str());
	strcpy_x(field.UserID, _req_login.user_name.c_str());
	strcpy_x(field.UserProductInfo, _req_login.broker.product_info.c_str());
	strcpy_x(field.AuthCode, _req_login.broker.auth_code.c_str());
	int ret = m_pTdApi->ReqAuthenticate(&field,++_requestID);
	if (0 != ret)
	{
		Log(LOG_INFO, NULL, "ctp ReqAuthenticate fail, instance=%p, ret=%d", this, ret);
		boost::unique_lock<boost::mutex> lock(_logInmutex);
		_logIn = false;
		_logInCondition.notify_all();
	}	
}

void traderctp::SendLoginRequest()
{
	long long now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
	m_req_login_dt.store(now);
	CThostFtdcReqUserLoginField field;
	memset(&field, 0, sizeof(field));
	strcpy_x(field.BrokerID, _req_login.broker.ctp_broker_id.c_str());
	strcpy_x(field.UserID, _req_login.user_name.c_str());
	strcpy_x(field.Password, _req_login.password.c_str());
	strcpy_x(field.UserProductInfo, _req_login.broker.product_info.c_str());
	strcpy_x(field.LoginRemark, _req_login.client_ip.c_str());
	int ret = m_pTdApi->ReqUserLogin(&field,++_requestID);
	if (0 != ret)
	{
		Log(LOG_INFO, NULL,
			"ctp ReqUserLogin fail, instance=%p, UserID=%s, LoginRemark=%s, ret=%d"
			, this, field.UserID, field.LoginRemark,ret);
		boost::unique_lock<boost::mutex> lock(_logInmutex);
		_logIn = false;
		_logInCondition.notify_all();
	}	
}

void traderctp::ProcessOnFrontDisconnected(int nReason)
{
	Log(LOG_WARNING, NULL
		, "ctp ProcessOnFrontDisconnected, instance=%p, nReason=%d, UserID=%s"
		, this
		, nReason
		, _req_login.user_name.c_str());
	OutputNotifyAllSycn(1,u8"已经断开与交易前置的连接");
}

void traderctp::OnFrontDisconnected(int nReason)
{
	//还在等待登录阶段
	if (!m_b_login.load())
	{
		Log(LOG_WARNING, NULL
			, "ctp OnFrontDisconnected, instance=%p, nReason=%d, UserID=%s"
			, this
			, nReason
			, _req_login.user_name.c_str());
		OutputNotifySycn(m_loging_connectId,1,u8"已经断开与交易前置的连接");
	}
	else
	{
		//这时不能直接调用
		_ios.post(boost::bind(&traderctp::ProcessOnFrontDisconnected
			,this,nReason));
	}
}

void traderctp::ProcessOnRspAuthenticate(std::shared_ptr<CThostFtdcRspInfoField> pRspInfo)
{
	if ((nullptr!=pRspInfo) && (pRspInfo->ErrorID != 0))
	{
		Log(LOG_WARNING, NULL, "ctp OnRspAuthenticate,instance=%p,UserID=%s,ErrorID=%d,ErrMsg=%s"
			,this,_req_login.user_name.c_str()
			,pRspInfo ? pRspInfo->ErrorID : -999
			,pRspInfo ? GBKToUTF8(pRspInfo->ErrorMsg).c_str() : "");
		return;
	}
	else
	{
		SendLoginRequest();
	}	
}

void traderctp::OnRspAuthenticate(CThostFtdcRspAuthenticateField *pRspAuthenticateField
	, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast)
{
	//还在等待登录阶段
	if (!m_b_login.load())
	{
		if ((nullptr != pRspInfo) && (pRspInfo->ErrorID != 0))
		{
			Log(LOG_WARNING, NULL, "ctp OnRspAuthenticate 1,instance=%p,UserID=%s,ErrorID=%d,ErrMsg=%s"
				, this, _req_login.user_name.c_str()
				, pRspInfo ? pRspInfo->ErrorID : -999
				, pRspInfo ? GBKToUTF8(pRspInfo->ErrorMsg).c_str() : ""
			);
			OutputNotifySycn(m_loging_connectId
				, pRspInfo->ErrorID,
				u8"交易服务器认证失败," + GBKToUTF8(pRspInfo->ErrorMsg), "WARNING");

			boost::unique_lock<boost::mutex> lock(_logInmutex);
			_logIn = false;
			_logInCondition.notify_all();
			return;
		}
		else
		{
			Log(LOG_WARNING, NULL, "ctp OnRspAuthenticate 2,instance=%p,UserID=%s,ErrorID=%d,ErrMsg=%s"
				, this, _req_login.user_name.c_str()
				, pRspInfo ? pRspInfo->ErrorID : -999
				, pRspInfo ? GBKToUTF8(pRspInfo->ErrorMsg).c_str() : ""
			);
			SendLoginRequest();
		}		
	}
	else
	{
		std::shared_ptr<CThostFtdcRspInfoField> ptr = nullptr;
		ptr = std::make_shared<CThostFtdcRspInfoField>(CThostFtdcRspInfoField(*pRspInfo));
		_ios.post(boost::bind(&traderctp::ProcessOnRspAuthenticate,this,ptr));
	}	
}

void traderctp::OnRspUserLogin(CThostFtdcRspUserLoginField* pRspUserLogin
	, CThostFtdcRspInfoField* pRspInfo, int nRequestID, bool bIsLast)
{
	//还在等待登录阶段
	if (!m_b_login.load())
	{
		Log(LOG_INFO, NULL, "ctp OnRspUserLogin, instance=%p, UserID=%s, ErrMsg=%s, TradingDay=%s, FrontId=%d, SessionId=%d, MaxOrderRef=%s"
			, this, _req_login.user_name.c_str(), GBKToUTF8(pRspInfo->ErrorMsg).c_str()
			, pRspUserLogin->TradingDay, pRspUserLogin->FrontID, pRspUserLogin->SessionID, pRspUserLogin->MaxOrderRef
		);
		m_position_ready = false;
		m_req_login_dt.store(0);		
		if (pRspInfo->ErrorID != 0)
		{
			Log(LOG_WARNING, NULL, "ctp OnRspUserLogin, instance=%p, UserID=%s, ErrorID=%d, ErrMsg=%s"
				, this, _req_login.user_name.c_str()
				, pRspInfo ? pRspInfo->ErrorID : -999
				, pRspInfo ? GBKToUTF8(pRspInfo->ErrorMsg).c_str() : ""
			);
			OutputNotifySycn(m_loging_connectId
				,pRspInfo->ErrorID,
				u8"交易服务器登录失败," + GBKToUTF8(pRspInfo->ErrorMsg), "WARNING");			
			boost::unique_lock<boost::mutex> lock(_logInmutex);
			_logIn = false;
			_logInCondition.notify_all();
			return;
		}
		else
		{
			Log(LOG_WARNING, NULL, "traderctp::OnRspUserLogin:%d", pRspInfo->ErrorID);
			std::string trading_day = pRspUserLogin->TradingDay;
			if (m_trading_day != trading_day)
			{
				m_ordermap_local_remote.clear();
				m_ordermap_remote_local.clear();
			}
			m_trading_day = trading_day;
			m_front_id = pRspUserLogin->FrontID;
			m_session_id = pRspUserLogin->SessionID;
			m_order_ref = atoi(pRspUserLogin->MaxOrderRef) + 1;
			OutputNotifySycn(m_loging_connectId, 0, u8"登录成功");
			AfterLogin();
			boost::unique_lock<boost::mutex> lock(_logInmutex);
			_logIn = true;
			_logInCondition.notify_all();
		}		
	}
	else
	{
		std::shared_ptr<CThostFtdcRspUserLoginField> ptr1 = nullptr;
		ptr1 = std::make_shared<CThostFtdcRspUserLoginField>(CThostFtdcRspUserLoginField(*pRspUserLogin));
		std::shared_ptr<CThostFtdcRspInfoField> ptr2 = nullptr;
		ptr2 = std::make_shared<CThostFtdcRspInfoField>(CThostFtdcRspInfoField(*pRspInfo));
		_ios.post(boost::bind(&traderctp::ProcessOnRspUserLogin, this,ptr1,ptr2));
	}		
}

void traderctp::ReinitCtp()
{
	if (nullptr != m_pTdApi)
	{
		StopTdApi();
	}
	boost::this_thread::sleep_for(boost::chrono::seconds(60));
	InitTdApi();	
}

void traderctp::ProcessOnRspUserLogin(std::shared_ptr<CThostFtdcRspUserLoginField> pRspUserLogin
	, std::shared_ptr<CThostFtdcRspInfoField> pRspInfo)
{
	Log(LOG_INFO, NULL, "ProcessOnRspUserLogin, instance=%p, UserID=%s, ErrMsg=%s, TradingDay=%s, FrontId=%d, SessionId=%d, MaxOrderRef=%s"
		, this, _req_login.user_name.c_str(), GBKToUTF8(pRspInfo->ErrorMsg).c_str()
		, pRspUserLogin->TradingDay, pRspUserLogin->FrontID, pRspUserLogin->SessionID, pRspUserLogin->MaxOrderRef
	);
	m_position_ready = false;
	m_req_login_dt.store(0);
	if (nullptr!=pRspInfo && pRspInfo->ErrorID != 0)
	{
		Log(LOG_WARNING, NULL, "ctp OnRspUserLogin, instance=%p, UserID=%s, ErrorID=%d, ErrMsg=%s"
			, this, _req_login.user_name.c_str()
			, pRspInfo ? pRspInfo->ErrorID : -999
			, pRspInfo ? GBKToUTF8(pRspInfo->ErrorMsg).c_str() : ""
		);
		OutputNotifyAllSycn(pRspInfo->ErrorID,
			u8"交易服务器重登录失败, " + GBKToUTF8(pRspInfo->ErrorMsg), "WARNING");	
		//如果不是密码错误
		if (3 != pRspInfo->ErrorID)
		{
			_ios.post(boost::bind(&traderctp::ReinitCtp, this));
		}		
		return;
	}
	else
	{
		std::string trading_day = pRspUserLogin->TradingDay;
		if (m_trading_day != trading_day)
		{
			m_ordermap_local_remote.clear();
			m_ordermap_remote_local.clear();
		}
		m_trading_day = trading_day;
		m_front_id = pRspUserLogin->FrontID;
		m_session_id = pRspUserLogin->SessionID;
		m_order_ref = atoi(pRspUserLogin->MaxOrderRef) + 1;
		OutputNotifyAllSycn(0, u8"交易服务器重登录成功");
	}	
}

void traderctp::ReqConfirmSettlement()
{
	CThostFtdcSettlementInfoConfirmField field;
	memset(&field, 0, sizeof(field));
	strcpy_x(field.BrokerID, m_broker_id.c_str());
	strcpy_x(field.InvestorID,_req_login.user_name.c_str());
	int r = m_pTdApi->ReqSettlementInfoConfirm(&field,0);
	Log(LOG_INFO, NULL,
		"ctp ReqConfirmSettlement, instance=%p, InvestorID=%s, ret=%d"
		, this, field.InvestorID, r);
}

void traderctp::ReqQrySettlementInfoConfirm()
{
	CThostFtdcQrySettlementInfoConfirmField field;
	memset(&field, 0, sizeof(field));
	strcpy_x(field.BrokerID, m_broker_id.c_str());
	strcpy_x(field.InvestorID, _req_login.user_name.c_str());
	strcpy_x(field.AccountID, _req_login.user_name.c_str());
	strcpy_x(field.CurrencyID, "CNY");
	int r = m_pTdApi->ReqQrySettlementInfoConfirm(&field, 0);
	Log(LOG_INFO, NULL, 
		"ctp ReqQrySettlementInfoConfirm, instance=%p, InvestorID=%s, ret=%d"
		, this, field.InvestorID, r);
}

void traderctp::ProcessQrySettlementInfoConfirm(std::shared_ptr<CThostFtdcSettlementInfoConfirmField> pSettlementInfoConfirm)
{	
	Log(LOG_INFO, NULL, "ctp ProcessQrySettlementInfoConfirm, instance=%p, UserID=%s, ConfirmDate=%s"
		, this,_req_login.user_name.c_str()
		, pSettlementInfoConfirm ? pSettlementInfoConfirm->ConfirmDate:"");
	if (pSettlementInfoConfirm && std::string(pSettlementInfoConfirm->ConfirmDate) >= m_trading_day)
	{
		return;
	}		
	m_need_query_settlement.store(true);
}

void traderctp::OnRspQrySettlementInfoConfirm(
	CThostFtdcSettlementInfoConfirmField *pSettlementInfoConfirm
	, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast)
{	
	std::shared_ptr<CThostFtdcSettlementInfoConfirmField> ptr = nullptr;
	if (nullptr != pSettlementInfoConfirm)
	{
		ptr == std::make_shared<CThostFtdcSettlementInfoConfirmField>(
			CThostFtdcSettlementInfoConfirmField(*pSettlementInfoConfirm));		
	}
	_ios.post(boost::bind(&traderctp::ProcessQrySettlementInfoConfirm,this,ptr));
}

void  traderctp::ProcessQrySettlementInfo(std::shared_ptr<CThostFtdcSettlementInfoField> pSettlementInfo, bool bIsLast)
{	
	if (bIsLast)
	{
		m_need_query_settlement.store(false);
		std::string str= GBKToUTF8(pSettlementInfo->Content);
		m_settlement_info += str;
		OutputNotifyAllSycn(0,m_settlement_info,"INFO","SETTLEMENT");		
	}
	else
	{
		std::string str = GBKToUTF8(pSettlementInfo->Content);
		m_settlement_info += str;		
	}	
}

void traderctp::OnRspQrySettlementInfo(CThostFtdcSettlementInfoField *pSettlementInfo, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast)
{
	if (!pSettlementInfo)
	{
		return;
	}	
	std::shared_ptr<CThostFtdcSettlementInfoField> ptr
		= std::make_shared<CThostFtdcSettlementInfoField>
		(CThostFtdcSettlementInfoField(*pSettlementInfo));
	_ios.post(boost::bind(&traderctp::ProcessQrySettlementInfo,this,ptr,bIsLast));	
}

void traderctp::ProcessUserPasswordUpdateField(std::shared_ptr<CThostFtdcUserPasswordUpdateField> pUserPasswordUpdate,
	std::shared_ptr<CThostFtdcRspInfoField> pRspInfo)
{
	Log(LOG_INFO, NULL, "ctp OnRspUserPasswordUpdate, instance=%p, UserID=%s"
		, this, _req_login.user_name.c_str());
	if (!pRspInfo)
		return;
	if (pRspInfo->ErrorID == 0)
	{
		OutputNotifySycn(m_loging_connectId,pRspInfo->ErrorID, u8"修改密码成功");
		if (_req_login.password == pUserPasswordUpdate->OldPassword)
		{
			_req_login.password = pUserPasswordUpdate->NewPassword;
		}		
	}
	else
	{
		OutputNotifySycn(m_loging_connectId, pRspInfo->ErrorID
			, u8"修改密码失败," + GBKToUTF8(pRspInfo->ErrorMsg)
			, "WARNING");
	}
}

void traderctp::OnRspUserPasswordUpdate(
	CThostFtdcUserPasswordUpdateField *pUserPasswordUpdate
	, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast)
{
	std::shared_ptr<CThostFtdcUserPasswordUpdateField> ptr1=nullptr;
	if (nullptr != pUserPasswordUpdate)
	{
		ptr1 = std::make_shared<CThostFtdcUserPasswordUpdateField>(
			CThostFtdcUserPasswordUpdateField(*pUserPasswordUpdate));
	}

	std::shared_ptr<CThostFtdcRspInfoField> ptr2 = nullptr;
	if (nullptr != pRspInfo)
	{
		ptr2 = std::make_shared<CThostFtdcRspInfoField>(
			CThostFtdcRspInfoField(*pRspInfo));
	}

	_ios.post(boost::bind(&traderctp::ProcessUserPasswordUpdateField
		, this, ptr1, ptr2));
	
}

bool traderctp::OrderIdLocalToRemote(const LocalOrderKey& local_order_key
	, RemoteOrderKey* remote_order_key)
{	
	if (local_order_key.order_id.empty()) 
	{
		remote_order_key->session_id = m_session_id;
		remote_order_key->front_id = m_front_id;
		remote_order_key->order_ref = std::to_string(m_order_ref++);
		return false;
	}
	auto it = m_ordermap_local_remote.find(local_order_key);
	if (it == m_ordermap_local_remote.end()) 
	{
		remote_order_key->session_id = m_session_id;
		remote_order_key->front_id = m_front_id;
		remote_order_key->order_ref = std::to_string(m_order_ref++);
		m_ordermap_local_remote[local_order_key] = *remote_order_key;
		m_ordermap_remote_local[*remote_order_key] = local_order_key;
		return false;
	}
	else 
	{
		*remote_order_key = it->second;
		return true;
	}
}

void traderctp::OrderIdRemoteToLocal(const RemoteOrderKey& remote_order_key
	, LocalOrderKey* local_order_key)
{
	auto it = m_ordermap_remote_local.find(remote_order_key);
	if (it == m_ordermap_remote_local.end()) 
	{
		char buf[1024];
		sprintf(buf, "SERVER.%s.%08x.%d"
			, remote_order_key.order_ref.c_str()
			, remote_order_key.session_id
			, remote_order_key.front_id);
		local_order_key->order_id = buf;
		local_order_key->user_id = _req_login.user_name;
		m_ordermap_local_remote[*local_order_key] = remote_order_key;
		m_ordermap_remote_local[remote_order_key] = *local_order_key;
	}
	else 
	{
		*local_order_key = it->second;
		RemoteOrderKey& r = const_cast<RemoteOrderKey&>(it->first);
		if (!remote_order_key.order_sys_id.empty())
			r.order_sys_id = remote_order_key.order_sys_id;
	}
}

void traderctp::FindLocalOrderId(const std::string& exchange_id
	, const std::string& order_sys_id, LocalOrderKey* local_order_key)
{
	for (auto it = m_ordermap_remote_local.begin()
		; it != m_ordermap_remote_local.end(); ++it) 
	{
		const RemoteOrderKey& rkey = it->first;
		if (rkey.order_sys_id == order_sys_id
			&& rkey.exchange_id == exchange_id) 
		{
			*local_order_key = it->second;
			return;
		}
	}
}

Order& traderctp::GetOrder(const std::string order_id)
{
	return m_data.m_orders[order_id];
}

void traderctp::ProcessRspOrderInsert(std::shared_ptr<CThostFtdcInputOrderField> pInputOrder
	, std::shared_ptr<CThostFtdcRspInfoField> pRspInfo)
{
	Log(LOG_INFO,NULL, "ctp OnRspOrderInsert, instance=%p, UserID=%s, ErrorID=%d"
		, this
		, _req_login.user_name.c_str()
		, pRspInfo ? pRspInfo->ErrorID : -999);
	if (pRspInfo && pRspInfo->ErrorID != 0)
	{
		std::stringstream ss;
		ss << m_front_id << m_session_id << pInputOrder->OrderRef;
		std::string strKey = ss.str();
		auto it = m_input_order_key_map.find(strKey);
		if (it != m_input_order_key_map.end())
		{
			//找到委托单
			RemoteOrderKey remote_key;
			remote_key.exchange_id = pInputOrder->ExchangeID;
			remote_key.instrument_id = pInputOrder->InstrumentID;
			remote_key.front_id = m_front_id;
			remote_key.session_id = m_session_id;
			remote_key.order_ref = pInputOrder->OrderRef;
			
			LocalOrderKey local_key;
			OrderIdRemoteToLocal(remote_key, &local_key);
					   
			Order& order = GetOrder(local_key.order_id);

			//委托单初始属性(由下单者在下单前确定, 不再改变)
			order.seqno = 0;
			order.user_id = local_key.user_id;
			order.order_id = local_key.order_id;
			order.exchange_id = pInputOrder->ExchangeID;
			order.instrument_id = pInputOrder->InstrumentID;
			switch (pInputOrder->Direction)
			{
			case THOST_FTDC_D_Buy:
				order.direction = kDirectionBuy;
				break;
			case THOST_FTDC_D_Sell:
				order.direction = kDirectionSell;
				break;
			default:
				break;
			}
			switch (pInputOrder->CombOffsetFlag[0])
			{
			case THOST_FTDC_OF_Open:
				order.offset = kOffsetOpen;
				break;
			case THOST_FTDC_OF_CloseToday:
				order.offset = kOffsetCloseToday;
				break;
			case THOST_FTDC_OF_Close:
			case THOST_FTDC_OF_CloseYesterday:
			case THOST_FTDC_OF_ForceOff:
			case THOST_FTDC_OF_LocalForceClose:
				order.offset = kOffsetClose;
				break;
			default:
				break;
			}
			order.volume_orign = pInputOrder->VolumeTotalOriginal;
			switch (pInputOrder->OrderPriceType)
			{
			case THOST_FTDC_OPT_AnyPrice:
				order.price_type = kPriceTypeAny;
				break;
			case THOST_FTDC_OPT_LimitPrice:
				order.price_type = kPriceTypeLimit;
				break;
			case THOST_FTDC_OPT_BestPrice:
				order.price_type = kPriceTypeBest;
				break;
			case THOST_FTDC_OPT_FiveLevelPrice:
				order.price_type = kPriceTypeFiveLevel;
				break;
			default:
				break;
			}
			order.limit_price = pInputOrder->LimitPrice;
			switch (pInputOrder->TimeCondition)
			{
			case THOST_FTDC_TC_IOC:
				order.time_condition = kOrderTimeConditionIOC;
				break;
			case THOST_FTDC_TC_GFS:
				order.time_condition = kOrderTimeConditionGFS;
				break;
			case THOST_FTDC_TC_GFD:
				order.time_condition = kOrderTimeConditionGFD;
				break;
			case THOST_FTDC_TC_GTD:
				order.time_condition = kOrderTimeConditionGTD;
				break;
			case THOST_FTDC_TC_GTC:
				order.time_condition = kOrderTimeConditionGTC;
				break;
			case THOST_FTDC_TC_GFA:
				order.time_condition = kOrderTimeConditionGFA;
				break;
			default:
				break;
			}
			switch (pInputOrder->VolumeCondition)
			{
			case THOST_FTDC_VC_AV:
				order.volume_condition = kOrderVolumeConditionAny;
				break;
			case THOST_FTDC_VC_MV:
				order.volume_condition = kOrderVolumeConditionMin;
				break;
			case THOST_FTDC_VC_CV:
				order.volume_condition = kOrderVolumeConditionAll;
				break;
			default:
				break;
			}
			//委托单当前状态
			order.volume_left = pInputOrder->VolumeTotalOriginal;
			order.status = kOrderStatusFinished;
			order.last_msg = GBKToUTF8(pRspInfo->ErrorMsg);
			order.changed = true;
			m_something_changed = true;
			SendUserData();

			OutputNotifyAllSycn(pRspInfo->ErrorID
				, u8"下单失败," + GBKToUTF8(pRspInfo->ErrorMsg), "WARNING");

			m_input_order_key_map.erase(it);
		}
	}	
}

void traderctp::OnRspOrderInsert(CThostFtdcInputOrderField* pInputOrder
	, CThostFtdcRspInfoField* pRspInfo, int nRequestID, bool bIsLast)
{
	std::shared_ptr<CThostFtdcInputOrderField> ptr1 = nullptr;
	if (nullptr != pInputOrder)
	{
		ptr1 = std::make_shared<CThostFtdcInputOrderField>(CThostFtdcInputOrderField(*pInputOrder));
	}
	std::shared_ptr<CThostFtdcRspInfoField> ptr2 = nullptr;
	if (nullptr != pRspInfo)
	{
		ptr2= std::make_shared<CThostFtdcRspInfoField>(CThostFtdcRspInfoField(*pRspInfo));
	}	
	_ios.post(boost::bind(&traderctp::ProcessRspOrderInsert,this,ptr1, ptr2));
	
}

void traderctp::ProcessOrderAction(std::shared_ptr<CThostFtdcInputOrderActionField> pInputOrderAction,
	std::shared_ptr<CThostFtdcRspInfoField> pRspInfo)
{
	Log(LOG_INFO, NULL, "ctp OnRspOrderAction, instance=%p, UserID=%s, ErrorID=%d"
		, this, _req_login.user_name.c_str(), pRspInfo ? pRspInfo->ErrorID : -999);
	if (pRspInfo->ErrorID != 0)
	{
		OutputNotifyAllSycn(pRspInfo->ErrorID
			, u8"撤单失败," + GBKToUTF8(pRspInfo->ErrorMsg), "WARNING");
	}
}

void traderctp::OnRspOrderAction(CThostFtdcInputOrderActionField* pInputOrderAction
	, CThostFtdcRspInfoField* pRspInfo, int nRequestID, bool bIsLast)
{
	std::shared_ptr<CThostFtdcInputOrderActionField> ptr1 = nullptr;
	if (nullptr != pInputOrderAction)
	{
		ptr1 = std::make_shared<CThostFtdcInputOrderActionField>(CThostFtdcInputOrderActionField(*pInputOrderAction));
	}
	std::shared_ptr<CThostFtdcRspInfoField> ptr2 = nullptr;
	if (nullptr != pRspInfo)
	{
		ptr2 = std::make_shared<CThostFtdcRspInfoField>(CThostFtdcRspInfoField(*pRspInfo));
	}
	_ios.post(boost::bind(&traderctp::ProcessOrderAction, this, ptr1, ptr2));	
}

void traderctp::ProcessErrRtnOrderInsert(std::shared_ptr<CThostFtdcInputOrderField> pInputOrder,
	std::shared_ptr<CThostFtdcRspInfoField> pRspInfo)
{	
	Log(LOG_INFO, NULL, "ctp ProcessErrRtnOrderInsert, instance=%p, UserID=%s, ErrorID=%d"
		, this
		, _req_login.user_name.c_str()
		, pRspInfo ? pRspInfo->ErrorID : -999);

	if (pInputOrder && pRspInfo && pRspInfo->ErrorID != 0)
	{		
		std::stringstream ss;
		ss << m_front_id << m_session_id << pInputOrder->OrderRef;
		std::string strKey = ss.str();
		auto it = m_input_order_key_map.find(strKey);
		if (it != m_input_order_key_map.end())
		{
			OutputNotifyAllSycn(pRspInfo->ErrorID
				, u8"下单失败," + GBKToUTF8(pRspInfo->ErrorMsg),"WARNING");

			//找到委托单
			RemoteOrderKey remote_key;
			remote_key.exchange_id = pInputOrder->ExchangeID;
			remote_key.instrument_id = pInputOrder->InstrumentID;
			remote_key.front_id = m_front_id;
			remote_key.session_id = m_session_id;
			remote_key.order_ref = pInputOrder->OrderRef;

			LocalOrderKey local_key;
			OrderIdRemoteToLocal(remote_key, &local_key);

			Order& order = GetOrder(local_key.order_id);

			//委托单初始属性(由下单者在下单前确定, 不再改变)
			order.seqno = 0;
			order.user_id = local_key.user_id;
			order.order_id = local_key.order_id;
			order.exchange_id = pInputOrder->ExchangeID;
			order.instrument_id = pInputOrder->InstrumentID;

			switch (pInputOrder->Direction)
			{
			case THOST_FTDC_D_Buy:
				order.direction = kDirectionBuy;
				break;
			case THOST_FTDC_D_Sell:
				order.direction = kDirectionSell;
				break;
			default:
				break;
			}

			switch (pInputOrder->CombOffsetFlag[0])
			{
			case THOST_FTDC_OF_Open:
				order.offset = kOffsetOpen;
				break;
			case THOST_FTDC_OF_CloseToday:
				order.offset = kOffsetCloseToday;
				break;
			case THOST_FTDC_OF_Close:
			case THOST_FTDC_OF_CloseYesterday:
			case THOST_FTDC_OF_ForceOff:
			case THOST_FTDC_OF_LocalForceClose:
				order.offset = kOffsetClose;
				break;
			default:
				break;
			}

			order.volume_orign = pInputOrder->VolumeTotalOriginal;
			switch (pInputOrder->OrderPriceType)
			{
			case THOST_FTDC_OPT_AnyPrice:
				order.price_type = kPriceTypeAny;
				break;
			case THOST_FTDC_OPT_LimitPrice:
				order.price_type = kPriceTypeLimit;
				break;
			case THOST_FTDC_OPT_BestPrice:
				order.price_type = kPriceTypeBest;
				break;
			case THOST_FTDC_OPT_FiveLevelPrice:
				order.price_type = kPriceTypeFiveLevel;
				break;
			default:
				break;
			}

			order.limit_price = pInputOrder->LimitPrice;
			switch (pInputOrder->TimeCondition)
			{
			case THOST_FTDC_TC_IOC:
				order.time_condition = kOrderTimeConditionIOC;
				break;
			case THOST_FTDC_TC_GFS:
				order.time_condition = kOrderTimeConditionGFS;
				break;
			case THOST_FTDC_TC_GFD:
				order.time_condition = kOrderTimeConditionGFD;
				break;
			case THOST_FTDC_TC_GTD:
				order.time_condition = kOrderTimeConditionGTD;
				break;
			case THOST_FTDC_TC_GTC:
				order.time_condition = kOrderTimeConditionGTC;
				break;
			case THOST_FTDC_TC_GFA:
				order.time_condition = kOrderTimeConditionGFA;
				break;
			default:
				break;
			}

			switch (pInputOrder->VolumeCondition)
			{
			case THOST_FTDC_VC_AV:
				order.volume_condition = kOrderVolumeConditionAny;
				break;
			case THOST_FTDC_VC_MV:
				order.volume_condition = kOrderVolumeConditionMin;
				break;
			case THOST_FTDC_VC_CV:
				order.volume_condition = kOrderVolumeConditionAll;
				break;
			default:
				break;
			}

			//委托单当前状态
			order.volume_left = pInputOrder->VolumeTotalOriginal;
			order.status = kOrderStatusFinished;
			order.last_msg = GBKToUTF8(pRspInfo->ErrorMsg);
			order.changed = true;
			m_something_changed = true;
			SendUserData();
			
			m_input_order_key_map.erase(it);			
		}		
	}
}

void traderctp::OnErrRtnOrderInsert(CThostFtdcInputOrderField *pInputOrder
	, CThostFtdcRspInfoField *pRspInfo)
{
	std::shared_ptr<CThostFtdcInputOrderField> ptr1 = nullptr;
	if (nullptr != pInputOrder)
	{
		ptr1 = std::make_shared<CThostFtdcInputOrderField>
			(CThostFtdcInputOrderField(*pInputOrder));
	}
	std::shared_ptr<CThostFtdcRspInfoField> ptr2 = nullptr;
	if (nullptr != pRspInfo)
	{
		ptr2 = std::make_shared<CThostFtdcRspInfoField>
			(CThostFtdcRspInfoField(*pRspInfo));
	}
	_ios.post(boost::bind(&traderctp::ProcessErrRtnOrderInsert
		, this, ptr1, ptr2));	
}

void traderctp::ProcessErrRtnOrderAction(std::shared_ptr<CThostFtdcOrderActionField> pOrderAction,
	std::shared_ptr<CThostFtdcRspInfoField> pRspInfo)
{
	Log(LOG_INFO, NULL, "ctp OnErrRtnOrderAction, instance=%p, UserID=%s, ErrorID=%d, ErrorMsg=%s"
		, this
		, _req_login.user_name.c_str()
		, pRspInfo ? pRspInfo->ErrorID : -999
		, pRspInfo ? GBKToUTF8(pRspInfo->ErrorMsg).c_str() : "");

	if (pOrderAction && pRspInfo && pRspInfo->ErrorID != 0)
	{
		std::stringstream ss;
		ss << m_front_id << m_session_id << pOrderAction->OrderRef;
		std::string strKey = ss.str();		
		auto it = m_action_order_map.find(strKey);
		if (it!= m_action_order_map.end())
		{
			OutputNotifyAllSycn(pRspInfo->ErrorID
				, u8"撤单失败," + GBKToUTF8(pRspInfo->ErrorMsg)
				, "WARNING");
			m_action_order_map.erase(it);
		}
	}
}

void traderctp::OnErrRtnOrderAction(CThostFtdcOrderActionField *pOrderAction, CThostFtdcRspInfoField *pRspInfo)
{
	std::shared_ptr<CThostFtdcOrderActionField> ptr1 = nullptr;
	if (nullptr != pOrderAction)
	{
		ptr1 = std::make_shared<CThostFtdcOrderActionField>
			(CThostFtdcOrderActionField(*pOrderAction));
	}
	std::shared_ptr<CThostFtdcRspInfoField> ptr2 = nullptr;
	if (nullptr != pRspInfo)
	{
		ptr2 = std::make_shared<CThostFtdcRspInfoField>
			(CThostFtdcRspInfoField(*pRspInfo));
	}
	_ios.post(boost::bind(&traderctp::ProcessErrRtnOrderAction, this, ptr1, ptr2));		
}

void traderctp::ProcessQryInvestorPosition(
	std::shared_ptr<CThostFtdcInvestorPositionField> pRspInvestorPosition,
	std::shared_ptr<CThostFtdcRspInfoField> pRspInfo, int nRequestID, bool bIsLast)
{
	if (pRspInvestorPosition)
	{
		Log(LOG_INFO, NULL
			, "traderctp ProcessQryInvestorPosition, instance=%p, nRequestID=%d, bIsLast=%d, UserID=%s, InstrumentId=%s, ExchangeId=%s"
			, this
			, nRequestID
			, bIsLast
			, _req_login.user_name.c_str()
			, pRspInvestorPosition->InstrumentID
			, pRspInvestorPosition->ExchangeID);
		std::string exchange_id = GuessExchangeId(pRspInvestorPosition->InstrumentID);
		std::string symbol = exchange_id + "." + pRspInvestorPosition->InstrumentID;
		auto ins = GetInstrument(symbol);
		if (!ins)
		{
			Log(LOG_WARNING, NULL
				, "ctp OnRspQryInvestorPosition, instrument not exist, instance=%p, UserID=%s, symbol=%s"
				, this
				, _req_login.user_name.c_str()
				, symbol.c_str());
		}
		else
		{
			Position& position = GetPosition(symbol);
			position.user_id = pRspInvestorPosition->InvestorID;
			position.exchange_id = exchange_id;
			position.instrument_id = pRspInvestorPosition->InstrumentID;
			if (pRspInvestorPosition->PosiDirection == THOST_FTDC_PD_Long)
			{
				if (pRspInvestorPosition->PositionDate == THOST_FTDC_PSD_Today)
				{
					position.volume_long_today = pRspInvestorPosition->Position;
					position.volume_long_frozen_today = pRspInvestorPosition->ShortFrozen;
					position.position_cost_long_today = pRspInvestorPosition->PositionCost;
					position.open_cost_long_today = pRspInvestorPosition->OpenCost;
					position.margin_long_today = pRspInvestorPosition->UseMargin;
				}
				else
				{
					position.volume_long_his = pRspInvestorPosition->Position;
					position.volume_long_frozen_his = pRspInvestorPosition->ShortFrozen;
					position.position_cost_long_his = pRspInvestorPosition->PositionCost;
					position.open_cost_long_his = pRspInvestorPosition->OpenCost;
					position.margin_long_his = pRspInvestorPosition->UseMargin;
				}
				position.position_cost_long = position.position_cost_long_today + position.position_cost_long_his;
				position.open_cost_long = position.open_cost_long_today + position.open_cost_long_his;
				position.margin_long = position.margin_long_today + position.margin_long_his;
			}
			else
			{
				if (pRspInvestorPosition->PositionDate == THOST_FTDC_PSD_Today)
				{
					position.volume_short_today = pRspInvestorPosition->Position;
					position.volume_short_frozen_today = pRspInvestorPosition->LongFrozen;
					position.position_cost_short_today = pRspInvestorPosition->PositionCost;
					position.open_cost_short_today = pRspInvestorPosition->OpenCost;
					position.margin_short_today = pRspInvestorPosition->UseMargin;
				}
				else
				{
					position.volume_short_his = pRspInvestorPosition->Position;
					position.volume_short_frozen_his = pRspInvestorPosition->LongFrozen;
					position.position_cost_short_his = pRspInvestorPosition->PositionCost;
					position.open_cost_short_his = pRspInvestorPosition->OpenCost;
					position.margin_short_his = pRspInvestorPosition->UseMargin;
				}
				position.position_cost_short = position.position_cost_short_today + position.position_cost_short_his;
				position.open_cost_short = position.open_cost_short_today + position.open_cost_short_his;
				position.margin_short = position.margin_short_today + position.margin_short_his;
			}
			position.changed = true;
		}
	}
	if (bIsLast) 
	{
		m_rsp_position_id.store(nRequestID);
		m_something_changed = true;
		m_position_ready = true;
		SendUserData();
	}
}

void traderctp::OnRspQryInvestorPosition(CThostFtdcInvestorPositionField* pInvestorPosition
	, CThostFtdcRspInfoField* pRspInfo, int nRequestID, bool bIsLast)
{
	std::shared_ptr<CThostFtdcInvestorPositionField> ptr1 = nullptr;
	if (nullptr != pInvestorPosition)
	{
		ptr1 = std::make_shared<CThostFtdcInvestorPositionField>
			(CThostFtdcInvestorPositionField(*pInvestorPosition));
	}
	std::shared_ptr<CThostFtdcRspInfoField> ptr2 = nullptr;
	if (nullptr != pRspInfo)
	{
		ptr2 = std::make_shared<CThostFtdcRspInfoField>
			(CThostFtdcRspInfoField(*pRspInfo));
	}
	_ios.post(boost::bind(&traderctp::ProcessQryInvestorPosition
		,this,ptr1, ptr2,nRequestID,bIsLast));
	
}

void traderctp::ProcessQryBrokerTradingParams(std::shared_ptr<CThostFtdcBrokerTradingParamsField> pBrokerTradingParams,
	std::shared_ptr<CThostFtdcRspInfoField> pRspInfo, int nRequestID, bool bIsLast)
{
	if (bIsLast)
	{
		m_need_query_broker_trading_params.store(false);
	}

	Log(LOG_INFO, NULL
		, "traderctp ProcessQryBrokerTradingParams, instance=%p, UserID=%s, ErrorID=%d"
		, this
		, _req_login.user_name.c_str()
		, pRspInfo ? pRspInfo->ErrorID : -999);

	if (!pBrokerTradingParams)
	{
		return;
	}

	Log(LOG_INFO, NULL
		, "BrokerTradingParams Algorithm:%d"
		, pBrokerTradingParams->Algorithm);
	m_Algorithm_Type = pBrokerTradingParams->Algorithm;	
}

void traderctp::ProcessQryTradingAccount(std::shared_ptr<CThostFtdcTradingAccountField> pRspInvestorAccount,
	std::shared_ptr<CThostFtdcRspInfoField> pRspInfo, int nRequestID, bool bIsLast)
{
	if (bIsLast) 
	{
		m_rsp_account_id.store(nRequestID);
	}

	Log(LOG_INFO, NULL
		, "traderctp ProcessQryTradingAccount, instance=%p, UserID=%s, ErrorID=%d"
		, this
		,_req_login.user_name.c_str()
		, pRspInfo ? pRspInfo->ErrorID : -999);

	if (!pRspInvestorAccount)
	{
		return;
	}	

	Account& account = GetAccount(pRspInvestorAccount->CurrencyID);

	//账号及币种
	account.user_id = pRspInvestorAccount->AccountID;

	account.currency = pRspInvestorAccount->CurrencyID;	
	//本交易日开盘前状态
	account.pre_balance = pRspInvestorAccount->PreBalance;	
	
	//本交易日内已发生事件的影响
	account.deposit = pRspInvestorAccount->Deposit;	
	
	account.withdraw = pRspInvestorAccount->Withdraw;	
	
	account.close_profit = pRspInvestorAccount->CloseProfit;	
	
	account.commission = pRspInvestorAccount->Commission;	
	
	account.premium = pRspInvestorAccount->CashIn;	
	
	account.static_balance = pRspInvestorAccount->PreBalance 
		- pRspInvestorAccount->PreCredit
		- pRspInvestorAccount->PreMortgage 
		+ pRspInvestorAccount->Mortgage
		- pRspInvestorAccount->Withdraw 
		+ pRspInvestorAccount->Deposit;	
	
	//当前持仓盈亏
	account.position_profit = pRspInvestorAccount->PositionProfit;
	
	account.float_profit = 0;
	//当前权益
	account.balance = pRspInvestorAccount->Balance;	
	
	//保证金占用, 冻结及风险度
	account.margin = pRspInvestorAccount->CurrMargin;	
	
	account.frozen_margin = pRspInvestorAccount->FrozenMargin;
	
	account.frozen_commission = pRspInvestorAccount->FrozenCommission;	
	
	account.frozen_premium = pRspInvestorAccount->FrozenCash;
	
	account.available = pRspInvestorAccount->Available;

	Log(LOG_INFO, NULL
		, "available ctp return:%f"
		, account.available);

	account.changed = true;
	if (bIsLast) 
	{
		m_something_changed = true;
		SendUserData();
	}
}

void traderctp::OnRspQryTradingAccount(CThostFtdcTradingAccountField* pRspInvestorAccount
	, CThostFtdcRspInfoField* pRspInfo, int nRequestID, bool bIsLast)
{
	std::shared_ptr<CThostFtdcTradingAccountField> ptr1 = nullptr;
	if (nullptr != pRspInvestorAccount)
	{
		ptr1 = std::make_shared<CThostFtdcTradingAccountField>(CThostFtdcTradingAccountField(*pRspInvestorAccount));
	}

	std::shared_ptr<CThostFtdcRspInfoField> ptr2 = nullptr;
	if (nullptr != pRspInfo)
	{
		ptr2 = std::make_shared<CThostFtdcRspInfoField>(CThostFtdcRspInfoField(*pRspInfo));
	}

	_ios.post(boost::bind(&traderctp::ProcessQryTradingAccount, this
		, ptr1, ptr2, nRequestID, bIsLast));
}

void traderctp::OnRspQryBrokerTradingParams(CThostFtdcBrokerTradingParamsField 
	*pBrokerTradingParams, CThostFtdcRspInfoField *pRspInfo
	, int nRequestID, bool bIsLast)
{
	std::shared_ptr<CThostFtdcBrokerTradingParamsField> ptr1 = nullptr;
	if (nullptr != pBrokerTradingParams)
	{
		ptr1 = std::make_shared<CThostFtdcBrokerTradingParamsField>
			(CThostFtdcBrokerTradingParamsField(*pBrokerTradingParams));
	}

	std::shared_ptr<CThostFtdcRspInfoField> ptr2 = nullptr;
	if (nullptr != pRspInfo)
	{
		ptr2 = std::make_shared<CThostFtdcRspInfoField>(CThostFtdcRspInfoField(*pRspInfo));
	}

	_ios.post(boost::bind(&traderctp::ProcessQryBrokerTradingParams, this
		, ptr1, ptr2, nRequestID, bIsLast));
}

void traderctp::ProcessQryContractBank(std::shared_ptr<CThostFtdcContractBankField> pContractBank,
	std::shared_ptr<CThostFtdcRspInfoField> pRspInfo, int nRequestID, bool bIsLast)
{
	Log(LOG_INFO, NULL
		, "traderctp ProcessQryContractBank, instance=%p, UserID=%s, ErrorID=%d"
		, this
		, _req_login.user_name.c_str()
		, pRspInfo ? pRspInfo->ErrorID : -999);

	if (!pContractBank) 
	{
		m_need_query_bank.store(false);
		return;
	}

	Bank& bank = GetBank(pContractBank->BankID);
	bank.bank_id = pContractBank->BankID;
	bank.bank_name = GBKToUTF8(pContractBank->BankName);
	Log(LOG_INFO, NULL
		,"traderctp ProcessQryContractBank,bank_id=%s,bank_name=%s"		
		,bank.bank_id.c_str()
		,bank.bank_name.c_str());

	if (bIsLast) 
	{
		m_need_query_bank.store(false);
	}	
}

void traderctp::OnRspQryContractBank(CThostFtdcContractBankField *pContractBank
	, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast)
{
	std::shared_ptr<CThostFtdcContractBankField> ptr1 = nullptr;
	if (nullptr != pContractBank)
	{
		ptr1 = std::make_shared<CThostFtdcContractBankField>
			(CThostFtdcContractBankField(*pContractBank));
	}

	std::shared_ptr<CThostFtdcRspInfoField> ptr2 = nullptr;
	if (nullptr != pRspInfo)
	{
		ptr2 = std::make_shared<CThostFtdcRspInfoField>
			(CThostFtdcRspInfoField(*pRspInfo));
	}

	_ios.post(boost::bind(&traderctp::ProcessQryContractBank, this
		, ptr1, ptr2, nRequestID, bIsLast));
}

void traderctp::ProcessQryAccountregister(std::shared_ptr<CThostFtdcAccountregisterField> pAccountregister,
	std::shared_ptr<CThostFtdcRspInfoField> pRspInfo, int nRequestID, bool bIsLast)
{
	Log(LOG_INFO, NULL
		, "traderctp ProcessQryAccountregister, instance=%p, UserID=%s, ErrorID=%d"
		, this
		,_req_login.user_name.c_str()
		, pRspInfo ? pRspInfo->ErrorID : -999);
	
	if (!pAccountregister) 
	{
		m_need_query_register.store(false);
		return;
	}	

	Log(LOG_INFO,NULL,"traderctp ProcessQryAccountregister,need query register");

	Bank& bank = GetBank(pAccountregister->BankID);
	bank.changed = true;
	if (bIsLast) 
	{
		m_need_query_register.store(false);
		m_something_changed = true;
		SendUserData();
	}
}

void traderctp::OnRspQryAccountregister(CThostFtdcAccountregisterField *pAccountregister
	, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast)
{
	std::shared_ptr<CThostFtdcAccountregisterField> ptr1 = nullptr;
	if (nullptr != pAccountregister)
	{
		ptr1 = std::make_shared<CThostFtdcAccountregisterField>(CThostFtdcAccountregisterField(*pAccountregister));
	}

	std::shared_ptr<CThostFtdcRspInfoField> ptr2 = nullptr;
	if (nullptr != pRspInfo)
	{
		ptr2 = std::make_shared<CThostFtdcRspInfoField>(CThostFtdcRspInfoField(*pRspInfo));
	}	

	_ios.post(boost::bind(&traderctp::ProcessQryAccountregister, this
		, ptr1, ptr2, nRequestID, bIsLast));
}

void traderctp::ProcessQryTransferSerial(std::shared_ptr<CThostFtdcTransferSerialField> pTransferSerial,
	std::shared_ptr<CThostFtdcRspInfoField> pRspInfo, int nRequestID, bool bIsLast)
{
	Log(LOG_INFO, NULL
		, "ctp OnRspQryTransferSerial, instance=%p, UserID=%s, ErrorID=%d"
		, this
		, _req_login.user_name.c_str()
		, pRspInfo ? pRspInfo->ErrorID : -999);
	if (!pTransferSerial)
	{
		return;
	}			

	TransferLog& d = GetTransferLog(std::to_string(pTransferSerial->PlateSerial));
	d.currency = pTransferSerial->CurrencyID;
	d.amount = pTransferSerial->TradeAmount;
	if (pTransferSerial->TradeCode == std::string("202002"))
		d.amount = 0 - d.amount;
	DateTime dt;
	dt.time.microsecond = 0;
	sscanf(pTransferSerial->TradeDate, "%04d%02d%02d", &dt.date.year, &dt.date.month, &dt.date.day);
	sscanf(pTransferSerial->TradeTime, "%02d:%02d:%02d", &dt.time.hour, &dt.time.minute, &dt.time.second);
	d.datetime = DateTimeToEpochNano(&dt);
	d.error_id = pTransferSerial->ErrorID;
	d.error_msg = GBKToUTF8(pTransferSerial->ErrorMsg);
	if (bIsLast) 
	{
		m_something_changed = true;
		SendUserData();
	}
}

void traderctp::OnRspQryTransferSerial(CThostFtdcTransferSerialField *pTransferSerial
	, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast)
{
	std::shared_ptr<CThostFtdcTransferSerialField> ptr1 = nullptr;
	if (nullptr != pTransferSerial)
	{
		ptr1 = std::make_shared<CThostFtdcTransferSerialField>
			(CThostFtdcTransferSerialField(*pTransferSerial));
	}

	std::shared_ptr<CThostFtdcRspInfoField> ptr2 = nullptr;
	if (nullptr != pRspInfo)
	{
		ptr2 = std::make_shared<CThostFtdcRspInfoField>
			(CThostFtdcRspInfoField(*pRspInfo));
	}

	_ios.post(boost::bind(&traderctp::ProcessQryTransferSerial, this
		, ptr1, ptr2, nRequestID, bIsLast));
}

void traderctp::ProcessFromBankToFutureByFuture(
	std::shared_ptr<CThostFtdcRspTransferField> pRspTransfer)
{
	if (!pRspTransfer)
	{
		return;
	}		

	Log(LOG_INFO, NULL
		, "ctp OnRtnFromBankToFutureByFuture, instance=%p, UserID=%s"
		, this
		,_req_login.user_name.c_str());

	if (pRspTransfer->ErrorID == 0) 
	{		
		TransferLog& d = GetTransferLog(std::to_string(pRspTransfer->PlateSerial));
		d.currency = pRspTransfer->CurrencyID;
		d.amount = pRspTransfer->TradeAmount;
		if (pRspTransfer->TradeCode == std::string("202002"))
			d.amount = 0 - d.amount;
		DateTime dt;
		dt.time.microsecond = 0;
		sscanf(pRspTransfer->TradeDate, "%04d%02d%02d", &dt.date.year, &dt.date.month, &dt.date.day);
		sscanf(pRspTransfer->TradeTime, "%02d:%02d:%02d", &dt.time.hour, &dt.time.minute, &dt.time.second);
		d.datetime = DateTimeToEpochNano(&dt);
		d.error_id = pRspTransfer->ErrorID;
		d.error_msg = GBKToUTF8(pRspTransfer->ErrorMsg);
		m_something_changed = true;
		SendUserData();
		m_req_account_id++;
	}
	else 
	{
		OutputNotifyAllSycn(pRspTransfer->ErrorID
			, u8"银期错误," + GBKToUTF8(pRspTransfer->ErrorMsg)
			, "WARNING");
	}
}

void traderctp::OnRtnFromBankToFutureByFuture(
	CThostFtdcRspTransferField *pRspTransfer)
{
	if (nullptr == pRspTransfer)
	{
		return;
	}
	std::shared_ptr<CThostFtdcRspTransferField> ptr1 = 
		std::make_shared<CThostFtdcRspTransferField>(
			CThostFtdcRspTransferField(*pRspTransfer));
	_ios.post(boost::bind(&traderctp::ProcessFromBankToFutureByFuture, this
		, ptr1));
}

void traderctp::OnRtnFromFutureToBankByFuture(CThostFtdcRspTransferField *pRspTransfer)
{
	if (nullptr == pRspTransfer)
	{
		return;
	}
	std::shared_ptr<CThostFtdcRspTransferField> ptr1=
		std::make_shared<CThostFtdcRspTransferField>(
			CThostFtdcRspTransferField(*pRspTransfer));
	_ios.post(boost::bind(&traderctp::ProcessFromBankToFutureByFuture, this
		, ptr1));
}

void traderctp::ProcessOnErrRtnBankToFutureByFuture(std::shared_ptr<CThostFtdcRspInfoField> pRspInfo)
{
	if (nullptr == pRspInfo)
	{
		return;
	}
	OutputNotifyAllAsych(pRspInfo->ErrorID
		, u8"银行资金转期货错误," + GBKToUTF8(pRspInfo->ErrorMsg)
		, "WARNING");
}

void traderctp::OnErrRtnBankToFutureByFuture(CThostFtdcReqTransferField *pReqTransfer, CThostFtdcRspInfoField *pRspInfo)
{
	if (nullptr == pRspInfo)
	{
		return;
	}
	if (0 == pRspInfo->ErrorID)
	{
		return;
	}
	std::shared_ptr<CThostFtdcRspInfoField> ptr2 = 
		std::make_shared<CThostFtdcRspInfoField>(CThostFtdcRspInfoField(*pRspInfo));
	_ios.post(boost::bind(&traderctp::ProcessOnErrRtnBankToFutureByFuture, this
		, ptr2));
}

void traderctp::ProcessnErrRtnFutureToBankByFuture(std::shared_ptr<CThostFtdcRspInfoField> pRspInfo)
{
	if (pRspInfo && pRspInfo->ErrorID != 0)
	{
		OutputNotifyAllSycn(pRspInfo->ErrorID
			, u8"期货资金转银行错误," + GBKToUTF8(pRspInfo->ErrorMsg), "WARNING");
	}
}

void traderctp::OnErrRtnFutureToBankByFuture(CThostFtdcReqTransferField *pReqTransfer, CThostFtdcRspInfoField *pRspInfo)
{
	if (nullptr == pRspInfo)
	{
		return;
	}
	std::shared_ptr<CThostFtdcRspInfoField> ptr2 = std::make_shared<CThostFtdcRspInfoField>(CThostFtdcRspInfoField(*pRspInfo));
	_ios.post(boost::bind(&traderctp::ProcessnErrRtnFutureToBankByFuture, this
		, ptr2));
}

void traderctp::ProcessRtnOrder(std::shared_ptr<CThostFtdcOrderField> pOrder)
{
	Log(LOG_INFO, NULL
		, "ctp OnRtnOrder, instance=%p, UserID=%s, InstrumentId=%s, OrderRef=%s, Session=%d"
		, this
		, _req_login.user_name.c_str()
		, pOrder->InstrumentID
		, pOrder->OrderRef
		, pOrder->SessionID);
	
	//找到委托单
	trader_dll::RemoteOrderKey remote_key;
	remote_key.exchange_id = pOrder->ExchangeID;
	remote_key.instrument_id = pOrder->InstrumentID;
	remote_key.front_id = pOrder->FrontID;
	remote_key.session_id = pOrder->SessionID;
	remote_key.order_ref = pOrder->OrderRef;
	remote_key.order_sys_id = pOrder->OrderSysID;
	trader_dll::LocalOrderKey local_key;
	OrderIdRemoteToLocal(remote_key, &local_key);
	Order& order = GetOrder(local_key.order_id);
	//委托单初始属性(由下单者在下单前确定, 不再改变)
	order.seqno = m_data_seq++;
	order.user_id = local_key.user_id;
	order.order_id = local_key.order_id;
	order.exchange_id = pOrder->ExchangeID;
	order.instrument_id = pOrder->InstrumentID;
	auto ins = GetInstrument(order.symbol());
	if (!ins)
	{
		Log(LOG_ERROR, NULL
			, "ctp OnRtnOrder, instrument not exist, instance=%p, UserID=%s, symbol=%s"
			, this
			, _req_login.user_name.c_str()
			,order.symbol().c_str());
		return;
	}
	switch (pOrder->Direction)
	{
	case THOST_FTDC_D_Buy:
		order.direction = kDirectionBuy;
		break;
	case THOST_FTDC_D_Sell:
		order.direction = kDirectionSell;
		break;
	default:
		break;
	}
	switch (pOrder->CombOffsetFlag[0])
	{
	case THOST_FTDC_OF_Open:
		order.offset = kOffsetOpen;
		break;
	case THOST_FTDC_OF_CloseToday:
		order.offset = kOffsetCloseToday;
		break;
	case THOST_FTDC_OF_Close:
	case THOST_FTDC_OF_CloseYesterday:
	case THOST_FTDC_OF_ForceOff:
	case THOST_FTDC_OF_LocalForceClose:
		order.offset = kOffsetClose;
		break;
	default:
		break;
	}
	order.volume_orign = pOrder->VolumeTotalOriginal;
	switch (pOrder->OrderPriceType)
	{
	case THOST_FTDC_OPT_AnyPrice:
		order.price_type = kPriceTypeAny;
		break;
	case THOST_FTDC_OPT_LimitPrice:
		order.price_type = kPriceTypeLimit;
		break;
	case THOST_FTDC_OPT_BestPrice:
		order.price_type = kPriceTypeBest;
		break;
	case THOST_FTDC_OPT_FiveLevelPrice:
		order.price_type = kPriceTypeFiveLevel;
		break;
	default:
		break;
	}
	order.limit_price = pOrder->LimitPrice;
	switch (pOrder->TimeCondition)
	{
	case THOST_FTDC_TC_IOC:
		order.time_condition = kOrderTimeConditionIOC;
		break;
	case THOST_FTDC_TC_GFS:
		order.time_condition = kOrderTimeConditionGFS;
		break;
	case THOST_FTDC_TC_GFD:
		order.time_condition = kOrderTimeConditionGFD;
		break;
	case THOST_FTDC_TC_GTD:
		order.time_condition = kOrderTimeConditionGTD;
		break;
	case THOST_FTDC_TC_GTC:
		order.time_condition = kOrderTimeConditionGTC;
		break;
	case THOST_FTDC_TC_GFA:
		order.time_condition = kOrderTimeConditionGFA;
		break;
	default:
		break;
	}
	switch (pOrder->VolumeCondition)
	{
	case THOST_FTDC_VC_AV:
		order.volume_condition = kOrderVolumeConditionAny;
		break;
	case THOST_FTDC_VC_MV:
		order.volume_condition = kOrderVolumeConditionMin;
		break;
	case THOST_FTDC_VC_CV:
		order.volume_condition = kOrderVolumeConditionAll;
		break;
	default:
		break;
	}
	//下单后获得的信息(由期货公司返回, 不会改变)
	DateTime dt;
	dt.time.microsecond = 0;
	sscanf(pOrder->InsertDate, "%04d%02d%02d", &dt.date.year, &dt.date.month, &dt.date.day);
	sscanf(pOrder->InsertTime, "%02d:%02d:%02d", &dt.time.hour, &dt.time.minute, &dt.time.second);
	order.insert_date_time = DateTimeToEpochNano(&dt);
	order.exchange_order_id = pOrder->OrderSysID;
	//委托单当前状态
	switch (pOrder->OrderStatus)
	{
	case THOST_FTDC_OST_AllTraded:
	case THOST_FTDC_OST_PartTradedNotQueueing:
	case THOST_FTDC_OST_NoTradeNotQueueing:
	case THOST_FTDC_OST_Canceled:
		order.status = kOrderStatusFinished;
		break;
	case THOST_FTDC_OST_PartTradedQueueing:
	case THOST_FTDC_OST_NoTradeQueueing:
	case THOST_FTDC_OST_Unknown:
		order.status = kOrderStatusAlive;
		break;
	default:
		break;
	}
	order.volume_left = pOrder->VolumeTotal;
	order.last_msg = GBKToUTF8(pOrder->StatusMsg);
	order.changed = true;
	//要求重新查询持仓
	m_req_position_id++;
	m_req_account_id++;
	m_something_changed = true;
	SendUserData();
	//发送下单成功通知
	if (pOrder->OrderStatus != THOST_FTDC_OST_Canceled
		&& pOrder->OrderStatus != THOST_FTDC_OST_Unknown
		&& pOrder->OrderStatus != THOST_FTDC_OST_NoTradeNotQueueing
		&& pOrder->OrderStatus != THOST_FTDC_OST_PartTradedNotQueueing
		) 
	{
		auto it = m_insert_order_set.find(pOrder->OrderRef);
		if (it != m_insert_order_set.end()) 
		{
			m_insert_order_set.erase(it);
			OutputNotifyAllSycn(1,u8"下单成功");
		}
	}
	
	if (pOrder->OrderStatus == THOST_FTDC_OST_Canceled
		&& pOrder->VolumeTotal > 0) 
	{
		auto it = m_cancel_order_set.find(order.order_id);
		if (it != m_cancel_order_set.end())
		{
			m_cancel_order_set.erase(it);
			OutputNotifyAllSycn(1,u8"撤单成功");
		}
		else
		{
			auto it2 = m_insert_order_set.find(pOrder->OrderRef);
			if (it2 != m_insert_order_set.end()) 
			{
				m_insert_order_set.erase(it2);
				OutputNotifyAllSycn(1,u8"下单失败," + order.last_msg, "WARNING");
			}
		}
	}
}

void traderctp::OnRtnOrder(CThostFtdcOrderField* pOrder)
{
	if (nullptr == pOrder)
	{
		return;
	}	
	std::shared_ptr<CThostFtdcOrderField> ptr2 =
		std::make_shared<CThostFtdcOrderField>(CThostFtdcOrderField(*pOrder));
	_ios.post(boost::bind(&traderctp::ProcessRtnOrder, this
		, ptr2));
}

void traderctp::ProcessRtnTrade(std::shared_ptr<CThostFtdcTradeField> pTrade)
{
	Log(LOG_INFO, NULL
		, "ctp OnRtnTrade, instance=%p, UserID=%s, InstrumentId=%s, OrderRef=%s"
		, this
		, _req_login.user_name.c_str()
		, pTrade->InstrumentID
		, pTrade->OrderRef);

	LocalOrderKey local_key;
	FindLocalOrderId(pTrade->ExchangeID, pTrade->OrderSysID, &local_key);
	std::string trade_key = local_key.order_id + "|" + std::string(pTrade->TradeID);
	Trade& trade = GetTrade(trade_key);
	trade.seqno = m_data_seq++;
	trade.trade_id = trade_key;
	trade.user_id = local_key.user_id;
	trade.order_id = local_key.order_id;
	trade.exchange_id = pTrade->ExchangeID;
	trade.instrument_id = pTrade->InstrumentID;
	trade.exchange_trade_id = pTrade->TradeID;
	auto ins = GetInstrument(trade.symbol());
	if (!ins) 
	{
		Log(LOG_ERROR, NULL,
			"ctp OnRtnTrade, instrument not exist, instance=%p, UserID=%s, symbol=%s"
			, this, _req_login.user_name.c_str(), trade.symbol().c_str());
		return;
	}

	switch (pTrade->Direction)
	{
	case THOST_FTDC_D_Buy:
		trade.direction = kDirectionBuy;
		break;
	case THOST_FTDC_D_Sell:
		trade.direction = kDirectionSell;
		break;
	default:
		break;
	}

	switch (pTrade->OffsetFlag)
	{
	case THOST_FTDC_OF_Open:
		trade.offset = kOffsetOpen;
		break;
	case THOST_FTDC_OF_CloseToday:
		trade.offset = kOffsetCloseToday;
		break;
	case THOST_FTDC_OF_Close:
	case THOST_FTDC_OF_CloseYesterday:
	case THOST_FTDC_OF_ForceOff:
	case THOST_FTDC_OF_LocalForceClose:
		trade.offset = kOffsetClose;
		break;
	default:
		break;
	}

	trade.volume = pTrade->Volume;
	trade.price = pTrade->Price;
	DateTime dt;
	dt.time.microsecond = 0;
	sscanf(pTrade->TradeDate, "%04d%02d%02d", &dt.date.year, &dt.date.month, &dt.date.day);
	sscanf(pTrade->TradeTime, "%02d:%02d:%02d", &dt.time.hour, &dt.time.minute, &dt.time.second);
	trade.trade_date_time = DateTimeToEpochNano(&dt);
	trade.commission = 0.0;
	trade.changed = true;
	m_something_changed = true;
	SendUserData();
}

void traderctp::OnRtnTrade(CThostFtdcTradeField* pTrade)
{
	if (nullptr == pTrade)
	{
		return;
	}
	std::shared_ptr<CThostFtdcTradeField> ptr2 = 
		std::make_shared<CThostFtdcTradeField>(CThostFtdcTradeField(*pTrade));
	_ios.post(boost::bind(&traderctp::ProcessRtnTrade,this,ptr2));
}

void traderctp::ProcessOnRtnTradingNotice(std::shared_ptr<CThostFtdcTradingNoticeInfoField> pTradingNoticeInfo)
{
	if (nullptr == pTradingNoticeInfo)
	{
		return;
	}

	auto s = GBKToUTF8(pTradingNoticeInfo->FieldContent);
	if (!s.empty())
	{
		Log(LOG_INFO
			, NULL
			, "ctp OnRtnTradingNotice,instance=%p,TradingNoticeInfo=%s"
			, this
			, s.c_str());
		OutputNotifyAllAsych(0,s);
	}
}

void traderctp::OnRtnTradingNotice(CThostFtdcTradingNoticeInfoField *pTradingNoticeInfo)
{
	if (nullptr == pTradingNoticeInfo)
	{
		return;
	}
	std::shared_ptr<CThostFtdcTradingNoticeInfoField> ptr
		= std::make_shared<CThostFtdcTradingNoticeInfoField>(CThostFtdcTradingNoticeInfoField(*pTradingNoticeInfo));
	_ios.post(boost::bind(&traderctp::ProcessOnRtnTradingNotice, this
		, ptr));
}

void traderctp::OnRspError(CThostFtdcRspInfoField* pRspInfo
	, int nRequestID, bool bIsLast)
{
	if (nullptr==pRspInfo)
	{
		return;
	}

	std::shared_ptr<CThostFtdcRspInfoField> ptr2 = 
		std::make_shared<CThostFtdcRspInfoField>(CThostFtdcRspInfoField(*pRspInfo));
	_ios.post(boost::bind(&traderctp::ProcessRspError,this
		, ptr2));	
}

void traderctp::ProcessRspError(std::shared_ptr<CThostFtdcRspInfoField> pRspInfo)
{
	if (nullptr != pRspInfo)
	{
		Log(LOG_INFO, NULL
			,"ctp OnRspError, instance=%p, UserID=%s, ErrMsg=%s"
			,this
			,_req_login.user_name.c_str()
			,GBKToUTF8(pRspInfo->ErrorMsg).c_str());
	}
}