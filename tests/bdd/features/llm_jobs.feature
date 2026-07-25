Feature: LLM job subsystem
  Background:
    Given a board with a card "Test title" in "To Do"

  Scenario: Submit a fake job and see status bar update
    When I launch the application
    And I press "T"
    Then the status bar should show "1 job running"

  Scenario: TUI stays responsive while LLM job is running
    When I launch the application
    And I press "T"
    Then I can still navigate with "j"

  Scenario: Job completes and status bar shows done
    When I launch the application
    And I press "T"
    And I wait for the job to complete
    Then the status bar should show "done"
