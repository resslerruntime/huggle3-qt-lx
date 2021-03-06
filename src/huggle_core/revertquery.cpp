//This program is free software: you can redistribute it and/or modify
//it under the terms of the GNU General Public License as published by
//the Free Software Foundation, either version 3 of the License, or
//(at your option) any later version.

//This program is distributed in the hope that it will be useful,
//but WITHOUT ANY WARRANTY; without even the implied warranty of
//MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//GNU General Public License for more details.

#include "revertquery.hpp"
#include "apiquery.hpp"
#include "apiqueryresult.hpp"
#include "configuration.hpp"
#include "exception.hpp"
#include "generic.hpp"
#include "querypool.hpp"
#include "localization.hpp"
#include "historyitem.hpp"
#include "hooks.hpp"
#include "syslog.hpp"
#include "wikisite.hpp"
#include "wikiuser.hpp"
#include "wikiutil.hpp"
#include <QUrl>
#include <QTimer>

using namespace Huggle;

RevertQuery::RevertQuery(WikiSite *site) : MediaWikiObject(site)
{
    this->Type = QueryRevert;
    this->SR_RevID = WIKI_UNKNOWN_REVID;
    this->Timeout = Configuration::HuggleConfiguration->SystemConfig_WriteTimeout;
}

RevertQuery::RevertQuery(WikiEdit *edit, WikiSite *site) : MediaWikiObject(site)
{
    this->Type = QueryRevert;
    this->editToBeReverted = edit;
    this->Timeout = Configuration::HuggleConfiguration->SystemConfig_WriteTimeout;
    this->SR_RevID = WIKI_UNKNOWN_REVID;
}

RevertQuery::~RevertQuery()
{
    if (this->timer != nullptr)
        this->timer->deleteLater();
    this->HI.Delete();
}

void RevertQuery::displayError(QString error, QString reason)
{
    if (reason.isEmpty())
        reason = error;
    Huggle::Syslog::HuggleLogs->ErrorLog(error);
    this->Kill();
    this->status = StatusDone;
    this->Result = new QueryResult();
    this->Result->SetError();
    this->processFailure();
}

QString RevertQuery::getCustomRevertStatus(bool *failed)
{
    ApiQueryResultNode *ms = this->qRevert->GetApiQueryResult()->GetNode("error");
    if (ms != nullptr)
    {
        if (ms->Attributes.contains("code"))
        {
            *failed = true;
            QString Error = ms->GetAttribute("code");
            if (Error == "alreadyrolled")
                return "Edit was reverted by someone else - skipping";
            if (Error == "onlyauthor")
                return "ERROR: Cannot rollback - page only has one author";
            if (Error == "badtoken")
            {
                QString msg = "ERROR: Cannot rollback, token " + this->GetSite()->GetProjectConfig()->Token_Rollback + " is not valid for some reason (mediawiki bug), trying once more";
                this->Suspend();
                Configuration::Logout(this->GetSite());
                return msg;
            }
            return "In error (" + Error +")";
        }
    }
    *failed = false;
    return "Reverted";
}

void RevertQuery::Process()
{
    if (!this->GetSite()->GetProjectConfig()->IsLoggedIn)
    {
        HUGGLE_DEBUG1("Postponing query " + QString::number(this->QueryID()) + " because the session is not valid");
        this->Suspend();
        return;
    }
    if (this->status == StatusProcessing)
    {
        Huggle::Syslog::HuggleLogs->DebugLog("Cowardly refusing to double process the query");
        return;
    }
    this->status = StatusProcessing;
    if (this->timer != nullptr)
        delete this->timer;
    this->StartTime = QDateTime::currentDateTime();
    this->timer = new QTimer(this);
    connect(this->timer, SIGNAL(timeout()), this, SLOT(OnTick()));
    this->timer->start(100);
    // we need to register the consumer here because of timer so that in case we decided to
    // decref this query while timer is still running we don't run to segfault
    this->RegisterConsumer(HUGGLECONSUMER_REVERTQUERYTMR);
    this->CustomStatus = _l("revert-preflightcheck");
    this->preflightCheck();
}

void RevertQuery::Restart()
{
    this->Kill();
    Query::Restart();
}

void RevertQuery::SetLast()
{
    this->oneEditOnly = true;
    this->usingSR = true;
}

void RevertQuery::Kill()
{
    if (this->qRevert != nullptr)
    {
        this->qRevert->Kill();
    }
    if (this->qPreflight != nullptr)
    {
        this->qPreflight->Kill();
        this->qPreflight.Delete();
    }
    if (this->qHistoryInfo != nullptr)
    {
        this->qHistoryInfo->Kill();
        this->qHistoryInfo.Delete();
    }
    // set the status
    this->status = StatusKilled;

    if (this->qRetrieve != nullptr)
    {
        this->qRetrieve->Kill();
        this->qRetrieve.Delete();
    }

    this->qRevert.Delete();
    this->freeResources();
    this->CustomStatus = "";
    this->preflightFinished = false;
    this->rollingBack = false;
}

WikiEdit *RevertQuery::GetEdit()
{
    return this->editToBeReverted;
}

bool RevertQuery::IsProcessed()
{
    if (this->status == StatusIsSuspended)
        return false;
    if (this->status == StatusDone || this->status == StatusInError || this->status == StatusKilled)
        return true;
    if (!this->preflightFinished)
        return false;
    return this->evaluateRevertQueryResults();
}

void RevertQuery::SetUsingSR(bool software_rollback)
{
    if (this->oneEditOnly)
        return;
    this->usingSR = software_rollback;
    return;
}

bool RevertQuery::IsUsingSR()
{
    return this->usingSR;
}

void RevertQuery::OnTick()
{
    if (this->status == StatusIsSuspended)
        return;
    if (this->status != StatusDone)
    {
        if (!this->preflightFinished)
        {
            this->evaluatePreflightCheck();
            return;
        }
        if (!this->rollingBack)
        {
            this->executeRollback();
            return;
        }
    }
    if (this->IsProcessed())
    {
        if (this->timer != nullptr)
        {
            this->timer->stop();
            delete this->timer;
            this->timer = nullptr;
        }
        this->UnregisterConsumer(HUGGLECONSUMER_REVERTQUERYTMR);
    }
}

QString RevertQuery::GetCustomRevertStatus(QueryResult *result_data, WikiSite *site, bool *failed, bool *suspend)
{
    ApiQueryResultNode *ms = (reinterpret_cast<ApiQueryResult*>(result_data)->GetNode("error"));
    *suspend = false;
    if (ms != nullptr)
    {
        if (ms->Attributes.contains("code"))
        {
            *failed = true;
            QString Error = ms->GetAttribute("code");
            if (Error == "alreadyrolled")
                return "Edit was reverted by someone else - skipping";
            if (Error == "onlyauthor")
                return "ERROR: Cannot rollback - page only has one author";
            if (Error == "badtoken")
            {
                QString msg = "ERROR: Cannot rollback, token " + site->GetProjectConfig()->Token_Rollback + " is not valid for some reason (mediawiki bug), please try it once more";
                *suspend = true;
                Configuration::Logout(site);
                return msg;
            }
            return "In error (" + Error +")";
        }
    }
    *failed = false;
    return "Reverted";
}

void RevertQuery::preflightCheck()
{
    // check if there is more edits in queue
    int index=0;
    bool failed = false;
    bool made_by_same_user = true;
    // we only need to check this in case we aren't to revert last edit only
    if (!this->oneEditOnly)
    {
        WikiEdit::Lock_EditList->lock();
        while (index < WikiEdit::EditList.count())
        {
            WikiEdit *w = WikiEdit::EditList.at(index++);
            if (!w->IsPostProcessed())
                continue;
            if (w != this->editToBeReverted)
            {
                if (!w->Page->EqualTo(this->editToBeReverted->Page))
                    continue;
                if (w->Time > this->editToBeReverted->Time)
                {
                    if (!w->User->EqualTo(this->editToBeReverted->User))
                        made_by_same_user = false;
                    failed = true;
                }
            }
        }
        WikiEdit::Lock_EditList->unlock();
    }
    if (failed)
    {
        if (Configuration::HuggleConfiguration->UserConfig->AutomaticallyResolveConflicts)
        {
            if (made_by_same_user && Configuration::HuggleConfiguration->UserConfig->RevertNewBySame)
            {
                // Conflict resolved: revert all edits including new edits made by same users
                this->IgnorePreflightCheck = true;
                Huggle::Syslog::HuggleLogs->Log(_l("cr-newer-edits", this->editToBeReverted->Page->PageName));
            } else
            {
                // Conflict resolved: do not perform any action - there are newer edits
                Huggle::Syslog::HuggleLogs->Log(_l("cr-stop-new-edit", this->editToBeReverted->Page->PageName));
                this->cancelRevert();
                return;
            }
        } else
        {
            QString text;
            if (made_by_same_user)
            {
                // There are new edits to " + PageName + ", are you sure you want to revert them?
                text = (_l("cr-message-new", this->editToBeReverted->Page->PageName));
            } else
            {
                // There are new edits made to " + PageName + " by a different user, are you sure you want
                // to revert them all? (it will likely fail anyway because of old token)
                text = (_l("cr-message-not-same", this->editToBeReverted->Page->PageName));
            }
            if (!Hooks::ShowYesNoQuestion(_l("revert-preflightcheck"), text, false))
            {
                this->cancelRevert();
                return;
            } else
            {
                this->IgnorePreflightCheck = true;
            }
        }
    }
    // now we need to retrieve the information about current status of page
    this->qPreflight = new ApiQuery(ActionQuery, this->GetSite());
    this->qPreflight->Parameters = "prop=revisions&rvprop=" + QUrl::toPercentEncoding("ids|flags|timestamp|user|userid|size|sha1|comment")
                                 + "&rvlimit=20&titles=" + QUrl::toPercentEncoding(this->editToBeReverted->Page->PageName);
    this->qPreflight->Process();
}

void RevertQuery::evaluatePreflightCheck()
{
    if (this->oneEditOnly || this->IgnorePreflightCheck)
    {
        this->preflightFinished = true;
        return;
    }
    if (this->qPreflight == nullptr || !this->qPreflight->IsProcessed())
        return;
    if (this->qPreflight->IsFailed())
    {
        Huggle::Syslog::HuggleLogs->Log(_l("revert-fail-pre-flight", this->qPreflight->GetFailureReason()));
        this->Kill();
        this->Result = new QueryResult();
        this->status = StatusDone;
        this->Result->SetError();
        this->processFailure();
        return;
    }
    QList<ApiQueryResultNode*> revs = this->qPreflight->GetApiQueryResult()->GetNodes("rev");
    bool made_by_same_user = true;
    bool multiple_edits = false;
    bool previous_edits_made_by_same_user = true;
    bool passed = true;
    foreach (ApiQueryResultNode *result, revs)
    {
        int revision_ID = WIKI_UNKNOWN_REVID;
        if (result->Attributes.contains("revid"))
        {
            revision_ID = result->GetAttribute("revid").toInt();
            if (editToBeReverted->RevID == revision_ID)
            {
                continue;
            }
        } else
        {
            continue;
        }
        if (result->Attributes.contains("user"))
        {
            QString user = WikiUtil::SanitizeUser(result->GetAttribute("user"));
            if (previous_edits_made_by_same_user && this->editToBeReverted->RevID != WIKI_UNKNOWN_REVID && revision_ID > this->editToBeReverted->RevID)
            {
                if (user != this->editToBeReverted->User->Username)
                {
                    previous_edits_made_by_same_user = false;
                }
                multiple_edits = previous_edits_made_by_same_user;
            }
        }
        if (this->editToBeReverted->RevID != WIKI_UNKNOWN_REVID && revision_ID > editToBeReverted->RevID)
        {
            passed = false;
        }
    }
    if (multiple_edits && Configuration::HuggleConfiguration->ProjectConfig->ConfirmMultipleEdits)
    {
        passed = false;
    }
    if (!passed)
    {
        QString text = ":)";
        if (multiple_edits)
        {
            // There are multiple edits by same user are you sure you want to revert them
            text = (_l("cr-message-same", this->editToBeReverted->Page->PageName));
        } else if (made_by_same_user)
        {
            // There are newer edits, are you sure you want to revert them
            text = (_l("cr-message-new", this->editToBeReverted->Page->PageName));
        } else
        {
            text = (_l("cr-message-not-same", this->editToBeReverted->Page->PageName));
        }
        if (Configuration::HuggleConfiguration->UserConfig->AutomaticallyResolveConflicts)
        {
            if (multiple_edits && !hcfg->UserConfig->RevertOnMultipleEdits)
            {
                Huggle::Syslog::HuggleLogs->Log(_l("cr-stop-multiple-same", this->editToBeReverted->Page->PageName));
                this->cancelRevert();
                return;
            } else if (multiple_edits && hcfg->UserConfig->RevertOnMultipleEdits)
            {
                // Conflict resolved: revert all edits - there are multiple edits by same user to
                Huggle::Syslog::HuggleLogs->Log(_l("cr-revert-same-user", this->editToBeReverted->Page->PageName));
            } else
            {
                if (previous_edits_made_by_same_user && Configuration::HuggleConfiguration->UserConfig->RevertNewBySame)
                {
                    Huggle::Syslog::HuggleLogs->Log(_l("cr-resolved-same-user", this->editToBeReverted->Page->PageName));
                } else
                {
                    // Conflict resolved: do not perform any action - there are newer edits to
                    Huggle::Syslog::HuggleLogs->Log(_l("cr-stop-new-edit", this->editToBeReverted->Page->PageName));
                    this->cancelRevert();
                    return;
                }
            }
        }
        if (!Hooks::ShowYesNoQuestion(_l("revert-preflightcheck"), text, false))
        {
            // abort
            this->freeResources();
            this->CustomStatus = "Stopped";
            this->Result = new QueryResult();
            this->Result->SetError("User requested to abort this");
            this->status = StatusDone;
            this->preflightFinished = true;
            this->processFailure();
            return;
        }
    }
    this->preflightFinished = true;
}

bool RevertQuery::evaluateRevertQueryResults()
{
    if (this->usingSR)
        return evaluateRevert();
    if (this->status == StatusIsSuspended)
        return false;
    if (this->qRevert == nullptr || !this->qRevert->IsProcessed())
        return false;
    bool failed = false;
    this->CustomStatus = this->getCustomRevertStatus(&failed);
    // In case that we got suspended by the session logout, quit here
    if (this->status == StatusIsSuspended)
        return false;
    if (!failed && this->qRevert->IsFailed())
    {
        failed = true;
        this->CustomStatus = this->qRevert->GetFailureReason();
    }
    if (failed)
    {
        this->status = StatusInError;
        Huggle::Syslog::HuggleLogs->Log(_l("revert-fail", this->qRevert->Target, this->CustomStatus));
        this->qRevert->Result->SetError(CustomStatus);
        this->Result = new QueryResult(true);
        this->Result->SetError(CustomStatus);
        this->processFailure();
    } else
    {
        this->status = StatusDone;
        HistoryItem *item = new HistoryItem(this->GetSite());
        this->HI = item;
        this->Result = new QueryResult();
        this->Result->Data = this->qRevert->Result->Data;
        item->Target = this->qRevert->Target;
        item->Type = HistoryRollback;
        item->Result = _l("successful");
        Hooks::WikiEdit_OnNewHistoryItem(item);
    }
    this->qRevert->UnregisterConsumer(HUGGLECONSUMER_REVERTQUERY);
    this->qRevert.Delete();
    return true;
}

void RevertQuery::cancelRevert()
{
    this->freeResources();
    this->CustomStatus = "Stopped";
    this->Result = new QueryResult(true);
    this->Result->SetError("User requested to abort this");
    this->status = StatusDone;
    this->preflightFinished = true;
    this->processFailure();
}

bool RevertQuery::evaluateRevert()
{
    if (this->eqSoftwareRollback != nullptr)
    {
        // we already reverted the page so check if we were successful in that
        if (!this->eqSoftwareRollback->IsProcessed())
        {
            // we are still reverting the page so quit this and wait
            return false;
        }
        this->Result = new QueryResult();
        if (this->eqSoftwareRollback->IsFailed())
        {
            // failure during revert
            Syslog::HuggleLogs->ErrorLog(_l("revert-fail", this->editToBeReverted->Page->PageName, "edit failed"));
            this->Result->SetError(this->eqSoftwareRollback->GetFailureReason());
            this->Kill();
            this->processFailure();
            this->status = StatusInError;
            return true;
        }
        Syslog::HuggleLogs->DebugLog("Sucessful SR of page " + this->editToBeReverted->Page->PageName);
        return true;
    }
    if (this->qRetrieve != nullptr)
    {
        // we are retrieving the content of previous edit made by a different user
        if (!this->qRetrieve->IsProcessed())
        {
            return false;
        }
        if (this->qRetrieve->IsFailed())
        {
            this->displayError("Unable to rollback the edit because previous content couldn't be retrieved");
            return true;
        }
        QString summary = this->Summary;
        QString content = "";
        ApiQueryResultNode* info = this->qRetrieve->GetApiQueryResult()->GetNode("rev");
        if (info == nullptr)
        {
            this->displayError("Unable to rollback the edit because previous content couldn't be retrieved");
            return true;
        }
        if (!info->Attributes.contains("revid"))
        {
            this->displayError("Unable to rollback the edit because query used to retrieve the content of previous"\
                               " version retrieved no RevID");
            return true;
        }
        QString rv = info->GetAttribute("revid");
        if (rv.toInt() != this->SR_RevID)
        {
            this->displayError("Unable to rollback the edit because query used to retrieve the content of previous"\
                               " version returned invalid RevID");
            return true;
        }
        content = info->Value;
        if (summary.isEmpty())
            summary = this->GetSite()->GetProjectConfig()->SoftwareRevertDefaultSummary;
        summary = summary.replace("$1", this->editToBeReverted->User->Username)
                .replace("$2", this->SR_Target)
                .replace("$3", QString::number(this->SR_Depth))
                .replace("$4", QString::number(this->SR_RevID));
        // we need to make sure there is edit suffix in revert summary for huggle
        summary = Configuration::GenerateSuffix(summary, this->GetSite()->GetProjectConfig());
        if (content.isEmpty())
        {
            /// \todo LOCALIZE ME
            this->displayError("Cowardly refusing to blank \"" + this->editToBeReverted->Page->PageName +
                               "\" software rollback was cancelled to prevent damage",
                               "content was resolved to blank edit");
            return true;
        }
        this->eqSoftwareRollback = WikiUtil::EditPage(this->editToBeReverted->Page, content, summary, this->MinorEdit);
        this->CustomStatus = _l("editing-page");
        return false;
    }
    if (this->qHistoryInfo == nullptr || !this->qHistoryInfo->IsProcessed())
        return false;
    if (this->qHistoryInfo->IsFailed())
    {
        this->displayError("Failed to retrieve a list of edits made to this page: " + this->qHistoryInfo->GetFailureReason());
        return true;
    }
    QList<ApiQueryResultNode *> revs = this->qHistoryInfo->GetApiQueryResult()->GetNodes("rev");
    // we need to find a first revision that is made by a different user
    // but first we need to check if last revision was actually made by this user, because if not
    // it's possible that someone else already reverted them
    if (revs.count() == 0)
    {
        // if we have absolutely no revisions in the result, it's pretty fucked
        this->displayError("Failed to retrieve a list of edits made to this page, query returned no data");
        return true;
    }
    // if the latest revid doesn't match our revid it means that someone made an edit
    bool passed = true;
    this->SR_Depth = 0;
    bool new_edits_resv = false;
    foreach (ApiQueryResultNode *e, revs)
    {
        if (e->Attributes.contains("revid"))
        {
            if (this->editToBeReverted->RevID == e->GetAttribute("revid").toInt())
                continue;
            if (this->editToBeReverted->RevID != WIKI_UNKNOWN_REVID && e->GetAttribute("revid").toInt() > this->editToBeReverted->RevID)
            {
                HUGGLE_DEBUG("RevID " + QString::number(e->GetAttribute("revid").toInt()) + " > " + QString::number(this->editToBeReverted->RevID), 2);
                if (Configuration::HuggleConfiguration->UserConfig->AutomaticallyResolveConflicts &&
                    Configuration::HuggleConfiguration->UserConfig->RevertNewBySame &&
                    e->Attributes.contains("user") &&
                    WikiUtil::SanitizeUser(e->GetAttribute("user")) == this->editToBeReverted->User->Username)
                {
                    // we want to automatically revert new edits that are made by same user
                    if (!new_edits_resv)
                    {
                        Huggle::Syslog::HuggleLogs->Log(_l("cr-newer-edits", this->editToBeReverted->Page->PageName));
                        // we don't want to bother users with this message for every resolved edit
                        // so we send it only once to logs
                        new_edits_resv = true;
                    }
                } else
                {
                    HUGGLE_DEBUG1("Newer edits found but no auto conflict resolution rule could be used");
                    passed = false;
                }
            }
        }
    }
    if (!passed)
    {
        this->displayError(_l("revert-cannotundo", this->editToBeReverted->Page->PageName));
        return true;
    }
    // now we need to find the first revision that was done by some different user
    //! \todo this list needs to be sorted by RevID
    foreach (ApiQueryResultNode *node, revs)
    {
        if (!node->Attributes.contains("revid") || !node->Attributes.contains("user"))
        {
            // this is fucked up piece of shit
            this->displayError("Unable to revert the page " + this->editToBeReverted->Page->PageName + " because mediawiki returned some non-sense");
            Huggle::Syslog::HuggleLogs->DebugLog("Nonsense: " + this->qHistoryInfo->Result->Data);
            return true;
        }
        QString sanitized = WikiUtil::SanitizeUser(node->GetAttribute("user"));
        // in case we are in depth higher than 0 (we passed out own edit) and we want to revert only 1 revision we exit
        if ((this->SR_Depth >= 1 && this->oneEditOnly) || sanitized != this->editToBeReverted->User->Username)
        {
            if (Configuration::HuggleConfiguration->Verbosity > 1 && sanitized != this->editToBeReverted->User->Username)
            {
                Syslog::HuggleLogs->DebugLog("found match for revert (depth " + QString::number(this->SR_Depth) + ") user "
                                             + sanitized + " != " + this->editToBeReverted->User->Username, 2);
            }
            // we got it, this is the revision we want to revert to
            this->SR_RevID = node->GetAttribute("revid").toInt();
            this->SR_Target = sanitized;
            break;
        }
        ++this->SR_Depth;
    }
    // let's check if depth isn't too low
    if (this->SR_Depth == 0)
    {
        // something is wrong
        this->displayError(_l("revert-fail", this->editToBeReverted->Page->PageName, "because it was edited meanwhile"));
        Syslog::HuggleLogs->DebugLog("revert failed because of 0 depth");
        return true;
    }
    if (this->SR_RevID == WIKI_UNKNOWN_REVID)
    {
        this->displayError(_l("revert-fail", this->editToBeReverted->Page->PageName, "because no previous version could be retrieved"));
        return true;
    }
    this->CustomStatus = "Retrieving content of previous version";
    // now we need to get the content of page
    this->qRetrieve = new ApiQuery(ActionQuery, this->GetSite());
    this->qRetrieve->Parameters = "prop=revisions&revids=" + QString::number(this->SR_RevID) + "&rvprop=" +
                                  QUrl::toPercentEncoding("ids|content");
    this->qRetrieve->Process();
    return false;
}

void RevertQuery::executeRollback()
{
    if (this->rollingBack)
    {
        Huggle::Exception::ThrowSoftException("Multiple request to rollback same query", BOOST_CURRENT_FUNCTION);
        return;
    }
    this->rollingBack = true;
    if (this->Summary.isEmpty())
        this->Summary = this->GetSite()->GetUserConfig()->RollbackSummaryUnknownTarget;
    if (this->Summary.contains("$1"))
        this->Summary = this->Summary.replace("$1", editToBeReverted->User->Username);
    // we need to make sure there is edit suffix in revert summary for huggle
    this->Summary = Configuration::GenerateSuffix(this->Summary, this->GetSite()->GetProjectConfig());
    this->editToBeReverted->User->SetBadnessScore(this->editToBeReverted->User->GetBadnessScore() + 200);
    WikiUser::UpdateUser(editToBeReverted->User);
    if (this->usingSR)
    {
        this->executeRevert();
        return;
    }
    if (!this->GetSite()->GetProjectConfig()->Rights.contains("rollback"))
    {
        Huggle::Syslog::HuggleLogs->Log(_l("software-rollback"));
        this->usingSR = true;
        this->executeRevert();
        return;
    }
    if (this->editToBeReverted->GetSite()->GetProjectConfig()->Token_Rollback.isEmpty())
    {
        Huggle::Syslog::HuggleLogs->ErrorLog(_l("revert-fail", this->editToBeReverted->Page->PageName, "rollback token was empty"));
        this->Result = new QueryResult();
        this->Result->SetError(_l("revert-fail", this->editToBeReverted->Page->PageName, "rollback token was empty"));
        this->status = StatusDone;
        this->freeResources();
        this->processFailure();
        return;
    }
    QString tag = "";
    if (Huggle::Version("1.28") <= this->GetSite()->MediawikiVersion && !this->GetSite()->GetProjectConfig()->Tag.isEmpty())
        tag = "&tags=" + QUrl::toPercentEncoding(this->GetSite()->GetProjectConfig()->Tag);
    this->qRevert = new ApiQuery(ActionRollback, this->GetSite());
    QString token = this->editToBeReverted->GetSite()->GetProjectConfig()->Token_Rollback;
    if (token.endsWith("+\\"))
    {
        token = QUrl::toPercentEncoding(token);
    }
    this->qRevert->Parameters = "title=" + QUrl::toPercentEncoding(editToBeReverted->Page->PageName) + "&token="
                + token + tag + "&watchlist=" + UserConfiguration::WatchListOptionToString(hcfg->UserConfig->Watchlist)
                + "&user=" + QUrl::toPercentEncoding(editToBeReverted->User->Username)
                + "&summary=" + QUrl::toPercentEncoding(this->Summary);
    this->qRevert->Target = editToBeReverted->Page->PageName;
    this->qRevert->UsingPOST = true;
    if (Configuration::HuggleConfiguration->Verbosity > 0)
        HUGGLE_QP_APPEND(this->qRevert);
    this->CustomStatus = _l("rollback", editToBeReverted->Page->PageName);
    Huggle::Syslog::HuggleLogs->DebugLog("Rolling back " + editToBeReverted->Page->PageName);
    this->qRevert->Process();
}

QString RevertQuery::QueryTargetToString()
{
    return this->editToBeReverted->Page->PageName;
}

void RevertQuery::executeRevert()
{
    // Get a list of edits made to this page
    this->qHistoryInfo = new ApiQuery(ActionQuery, this->GetSite());
    this->qHistoryInfo->Parameters = "prop=revisions&rvprop=" + QUrl::toPercentEncoding("ids|flags|timestamp|user|userid|content|size|sha1|comment")
                                    + "&rvlimit=20&titles=" + QUrl::toPercentEncoding(this->editToBeReverted->Page->PageName);
    this->qHistoryInfo->Process();
}

void RevertQuery::freeResources()
{
    if (this->timer != nullptr)
    {
        this->timer->stop();
        delete this->timer;
        this->timer = nullptr;
    }
    this->eqSoftwareRollback.Delete();
    this->UnregisterConsumer(HUGGLECONSUMER_REVERTQUERYTMR);
    this->qHistoryInfo.Delete();
    this->qRevert.Delete();
    this->qPreflight.Delete();
    this->qRetrieve.Delete();
}
