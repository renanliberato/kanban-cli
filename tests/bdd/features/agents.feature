Feature: Agent Primitives
  As a user
  I want to define agents and trigger them via @mentions in comments
  So that AI agents can inspect tasks and add comments or update descriptions

  Background:
    Given a board with a card "test" in "To Do"
    And an agent "analytical" of type "comment"

  Scenario: Unknown @mention is ignored
    When I launch the application
    And I press Enter
    And I press "c"
    And I type "@nonexistent check this"
    And I press Enter
    Then the screen should contain "No agent named"

  Scenario: Listing agents via CLI
    When I run "agents" with no arguments
    Then the exit code should be 0
    And the output should contain "analytical"

  Scenario: CLI comment with @mention triggers agent
    When I run "comment" with arguments "1" "@analytical check this"
    Then the exit code should be 0

  Scenario: Shipped comment agent triggered via @mention in TUI
    Given the shipped agent "planner" is installed
    When I launch the application
    And I press Enter
    And I press "c"
    And I type "@planner break this down"
    And I press Enter
    Then the screen should contain "Running @planner"
    When I wait for the agent job to complete
    And I press Enter
    Then the screen should contain "@planner commented"

  Scenario: Shipped description agent triggered via @mention in TUI
    Given the shipped agent "writer" is installed
    When I launch the application
    And I press Enter
    And I press "c"
    And I type "@writer rewrite this"
    And I press Enter
    Then the screen should contain "Running @writer"
    When I wait for the agent job to complete
    Then the screen should contain "@writer updated the task"

  Scenario: kanban agents lists shipped agents with correct types
    Given the shipped agent "planner" is installed
    And the shipped agent "writer" is installed
    When I run "agents" with no arguments
    Then the exit code should be 0
    And the output should contain "planner"
    And the output should contain "comment"
    And the output should contain "writer"
    And the output should contain "description"

  Scenario: Multi-line comment body renders correctly in detail view
    Given a board with a card "multiline" in "To Do"
    When I launch the application
    And I quit the application
    When I add a multi-line comment "First paragraph\nSecond paragraph" on card 1
    When I launch the application
    And I press Enter
    Then the screen should contain "First paragraph"
    And the screen should contain "Second paragraph"
