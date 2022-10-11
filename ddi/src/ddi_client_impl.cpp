#include <chrono>
#include <memory>
#include <thread>
#include <string>
#include <utility>
#include <fstream>

#define RAPIDJSON_HAS_STDSTRING 1

#include "rapidjson/document.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"

#include "ddi_client_impl.hpp"
#include "response_impl.hpp"
#include "actions_impl.hpp"
#include "utils.hpp"


namespace ddi {

    const char *AUTHORIZATION_HEADER = "Authorization";
    const char *GATEWAY_TOKEN_HEADER = "GatewayToken";
    const char *TARGET_TOKEN_HEADER = "TargetToken";

    void checkHttpCode(int presented, int expected) {
        if (presented == HTTP_UNAUTHORIZED)
            throw unauthorized_exception();
        if (presented != expected)
            throw http_unexpected_code_exception(presented, expected);
    }

    class AuthRestoreHandler_ : public AuthRestoreHandler {
        HawkbitCommunicationClient *cli;
    public:
        explicit AuthRestoreHandler_(HawkbitCommunicationClient *cli_) : cli(cli_) {}

        void setTLS(const std::string &crt, const std::string &key) override {
            cli->setTLS(crt, key);
        }

        void setEndpoint(const std::string &endpoint) override {
            cli->setEndpoint(endpoint);
        }

        void setDeviceToken(const std::string &token) override {
            cli->setDeviceToken(token);
        }

        void setGatewayToken(const std::string &token) override {
            cli->setGatewayToken(token);
        }

        void setEndpoint(std::string &hawkbitEndpoint, const std::string &controllerId,
                         const std::string &tenant = "default") override {

            cli->setEndpoint(hawkbitEndpoint, controllerId, tenant);
        };
    };

    void HawkbitCommunicationClient::stop() {

        isRunning = false;
    }

     void HawkbitCommunicationClient::run() {
        if (hawkbitURI.isEmpty()) {
            if (!authErrorHandler)  throw client_initialize_error("endpoint or AuthErrorHandler is not set");
            authErrorHandler->onAuthError(
                    std::make_unique<AuthRestoreHandler_>(this));
        }

        isRunning = true;

        while (true) {
            ignoreSleep = false;
            doPoll();

            if (!isRunning)
                break;

            if (!ignoreSleep && currentSleepTime > 0)
                std::this_thread::sleep_for(std::chrono::milliseconds(currentSleepTime));
        }
    }

    httpclient::Client HawkbitCommunicationClient::newHttpClient(uri::URI &hostEndpoint) const {
        // key pair auth
        if (mTLSKeypair.isSet) {
            return httpclient::Client(
                hostEndpoint.getScheme() + "://" + hostEndpoint.getAuthority(),
                std::make_unique<httpclient::mTLSKeyPair>(mTLSKeypair.crt, mTLSKeypair.key));
        }

        auto cli = httpclient::Client(hostEndpoint.getScheme() + "://" + hostEndpoint.getAuthority());
        cli.enable_server_certificate_verification(serverCertificateVerify);

        return cli;
    }

    // set actionId here (hawkbit api requires it but in docs not)
    void fillResponseDocument(Response *response, rapidjson::Document &document, int actionId) {
        if (response == nullptr) {
            throw wrong_response();
        }

        rapidjson::Value status(rapidjson::kObjectType);
        rapidjson::Value result(rapidjson::kObjectType);

        result.AddMember("finished", Response::finishedToString(response->getFinished()), document.GetAllocator());
        status.AddMember("result", result, document.GetAllocator());
        status.AddMember("execution", Response::executionToString(response->getExecution()), document.GetAllocator());

        rapidjson::Value details(rapidjson::kArrayType);
        for (const auto &val: response->getDetails()) {
            details.PushBack(rapidjson::Value{}.SetString(val.c_str(), val.length(), document.GetAllocator()),
                             document.GetAllocator());
        }
        status.AddMember("details", details, document.GetAllocator());

        document.AddMember("status", status, document.GetAllocator());

        if (actionId >= 0) {
            document.AddMember("id", std::to_string(actionId), document.GetAllocator());
        }
    }

    void HawkbitCommunicationClient::followConfigData(uri::URI &followURI) {
        auto req = handler->onConfigRequest();
        auto requestData = req->getData();
        if (requestData.empty()) {
            return;
        }

        rapidjson::Document document;
        document.SetObject();

        // fill data object
        rapidjson::Value data(rapidjson::kObjectType);
        for (auto &val: requestData) {
            rapidjson::Value key(val.first, document.GetAllocator());
            rapidjson::Value value(val.second, document.GetAllocator());
            data.AddMember(key, value, document.GetAllocator());
        }

        document.AddMember("data", data, document.GetAllocator());
        document.AddMember("mode", ConfigResponse::modeToString(req->getMode()), document.GetAllocator());
        auto builder = ResponseBuilder::newInstance();
        auto resp = builder->setFinished(Response::SUCCESS)->setExecution(Response::CLOSED)->build();

        fillResponseDocument(resp.get(), document, -1);

        rapidjson::StringBuffer buf;
        rapidjson::Writer<rapidjson::StringBuffer> writer(buf);
        document.Accept(writer);

        retryHandler(followURI, [&](httpclient::Client &cli) {
            return cli.Put(followURI.getPath().c_str(), defaultHeaders, buf.GetString(), "application/json");
        });

        ignoreSleep = req->isIgnoredSleep();
    }

    // see documentation: https://www.eclipse.org/hawkbit/rest-api/rootcontroller-api-guide/#_post_tenant_controller_v1_controllerid_cancelaction_actionid_feedback
    std::string formatFeedbackPath(uri::URI uri) {
        auto path = uri.getPath();
        if (path[path.length() - 1] != '/') {
            path += "/";
        }
        path += "feedback";
        return path;
    }

    void HawkbitCommunicationClient::followCancelAction(uri::URI &followURI) {
        auto resp = retryHandler(followURI, [&](httpclient::Client &cli) {
            return cli.Get(followURI.getPath().c_str(), defaultHeaders);
        });
        auto cancelAction = CancelAction_::fromString(resp->body);
        auto actionId = cancelAction->getId();
        auto cliResp = handler->onCancelAction(std::move(cancelAction));

        rapidjson::Document document;
        document.SetObject();

        fillResponseDocument(cliResp.get(), document, actionId);

        rapidjson::StringBuffer buf;
        rapidjson::Writer<rapidjson::StringBuffer> writer(buf);
        document.Accept(writer);
        try {
            retryHandler(followURI, [&](httpclient::Client &cli) {
                return cli.Post(formatFeedbackPath(
                        followURI).c_str(), defaultHeaders, buf.GetString(), "application/json");
            });

            if (cliResp->getDeliveryListener()) {
                cliResp->getDeliveryListener()->onSuccessfulDelivery();
            }
            // catch only error http code, if no handler defined pass through
        } catch (http_unexpected_code_exception &e) {
            if (cliResp->getDeliveryListener()) {
                cliResp->getDeliveryListener()->onError();
            } else {
                throw e;
            }
        }

        ignoreSleep = cliResp->isIgnoredSleep();
    }

    void HawkbitCommunicationClient::followDeploymentBase(uri::URI &followURI) {
        auto resp = retryHandler(followURI, [&](httpclient::Client &cli) {
            return cli.Get(followURI.getPath().c_str(), defaultHeaders);
        });

        auto deploymentBase = DeploymentBase_::from(resp->body, this);
        auto actionId = deploymentBase->getId();
        auto cliResp = handler->onDeploymentAction(std::move(deploymentBase));

        rapidjson::Document document;
        document.SetObject();
        fillResponseDocument(cliResp.get(), document, actionId);

        rapidjson::StringBuffer buf;
        rapidjson::Writer<rapidjson::StringBuffer> writer(buf);
        document.Accept(writer);
        try {
            retryHandler(followURI, [&](httpclient::Client &cli) {
                return cli.Post(
                        formatFeedbackPath(followURI).c_str(), defaultHeaders, buf.GetString(), "application/json");
            });

            if (cliResp->getDeliveryListener()) {
                cliResp->getDeliveryListener()->onSuccessfulDelivery();
            }
            // catch only error http code, if no handler defined pass through
        } catch (http_unexpected_code_exception &e) {
            if (cliResp->getDeliveryListener()) {
                cliResp->getDeliveryListener()->onError();
            } else {
                throw e;
            }
        }

        ignoreSleep = cliResp->isIgnoredSleep();
    }

    void HawkbitCommunicationClient::doPoll() {
        // firstly do GET request to default endpoint. hawkBit send meta for next poll and
        //  action list to follow
        auto resp = retryHandler(hawkbitURI, [&](httpclient::Client &cli) {
            return cli.Get(hawkbitURI.getPath().c_str(), defaultHeaders);
        });
        auto polingData = PollingData_::fromString(resp->body);
        // handle if sleepTime not defined by hawkBit
        currentSleepTime = (polingData->getSleepTime() > 0) ? polingData->getSleepTime() : defaultSleepTime;
        auto followURI = polingData->getFollowURI();
        switch (polingData->getAction()) {
            case Actions_::NONE:
                return handler->onNoActions();
            case Actions_::GET_CONFIG_DATA:
                return followConfigData(followURI);
            case CANCEL_ACTION:
                return followCancelAction(followURI);
            case DEPLOYMENT_BASE:
                return followDeploymentBase(followURI);
        }
    }

    void HawkbitCommunicationClient::downloadTo(uri::URI downloadURI, const std::string &path) {
        std::ofstream file(path, std::ios::binary);
        retryHandler(downloadURI, [&](httpclient::Client &cli) {
            return cli.Get(downloadURI.getPath().c_str(), defaultHeaders,
                           [](const httpclient::Response &r) {
                               checkHttpCode(r.status, HTTP_OK);
                               return true;
                           },
                           [&](const char *data, size_t size) {
                               file.write(data, size);
                               return !file.bad();
                           }
            );
        });

    }

    std::string HawkbitCommunicationClient::getBody(uri::URI downloadURI) {
        return retryHandler(downloadURI, [&](httpclient::Client &cli) {
            return cli.Get(downloadURI.getPath().c_str(), defaultHeaders);
        })->body;
    }

    void HawkbitCommunicationClient::downloadWithReceiver(uri::URI downloadURI,
                                                          std::function<bool(const char *, size_t)> func) {
        retryHandler(downloadURI, [&](httpclient::Client &cli) {
            return cli.Get(downloadURI.getPath().c_str(), defaultHeaders,
                           [](const httpclient::Response &r) {
                               checkHttpCode(r.status, HTTP_OK);
                               return true;
                           }, func
            );
        });

    }

    httpclient::Result HawkbitCommunicationClient::wrappedRequest(uri::URI reqUri, const std::function<httpclient::Result(
            httpclient::Client &)> &func) {
        auto cli = newHttpClient(reqUri);
        auto resp = func(cli);
        if (resp.error() != httpclient::Error::Success) {
            throw http_lib_error((int) resp.error());
        }
        checkHttpCode(resp->status, HTTP_OK);
        return resp;
    }

    httpclient::Result HawkbitCommunicationClient::retryHandler(uri::URI reqUri, const std::function<httpclient::Result(
            httpclient::Client &)> &func) {
        try {
            return wrappedRequest(reqUri, func);
        } catch (unauthorized_exception &e) {
            if (!authErrorHandler) throw e;
            authErrorHandler->onAuthError(
                    std::make_unique<AuthRestoreHandler_>(this));
        }

        return wrappedRequest(reqUri, func);
    }

    void HawkbitCommunicationClient::setTLS(const std::string &crt, const std::string &key) {
        mTLSKeypair.isSet = true;
        mTLSKeypair.crt = crt;
        mTLSKeypair.key = key;
        defaultHeaders.erase(AUTHORIZATION_HEADER);
    }

    std::string formatAuthHeader(const std::string &authType, const std::string &val) {
        return authType + " " + val;
    }

    void HawkbitCommunicationClient::setEndpoint(const std::string &endpoint) {
        hawkbitURI = uri::URI::fromString(endpoint);
    }

    void HawkbitCommunicationClient::setDeviceToken(const std::string &token) {
        defaultHeaders.insert({AUTHORIZATION_HEADER,
                               formatAuthHeader(TARGET_TOKEN_HEADER, token)});
        mTLSKeypair.isSet = false;
        mTLSKeypair.crt = "";
        mTLSKeypair.key = "";
    }

    void HawkbitCommunicationClient::setGatewayToken(const std::string &token) {
        defaultHeaders.insert({AUTHORIZATION_HEADER,
                               formatAuthHeader(GATEWAY_TOKEN_HEADER, token)});
        mTLSKeypair.isSet = false;
        mTLSKeypair.crt = "";
        mTLSKeypair.key = "";
    }

    void HawkbitCommunicationClient::setEndpoint(std::string &hawkbitEndpoint, const std::string &controllerId,
                                                 const std::string &tenant) {

        setEndpoint(hawkbitEndpointFrom(hawkbitEndpoint, controllerId, tenant));
    }
}