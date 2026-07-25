Feature: Persistence
  As a user
  I want my board to persist across application restarts
  So that I do not lose my work

  Scenario: Quit and restart with same file shows the same board
    Given a board with the following cards
      | title | column |
      | alpha | To Do  |
      | beta  | To Do  |
      | gamma | Doing  |
    When I launch the application
    # Add a card
    When I add a card "delta" to "To Do"
    # Delete a card
    When I delete the card "gamma"
    And I confirm the deletion
    # Move a card
    When I move the card "beta" right
    # Quit and restart
    Then after restart the board should match
      | column | titles          |
      | To Do  | alpha, delta    |
      | Doing  | beta            |
      | Done   |                 |

  Scenario: Adding cards persists after quit and restart
    Given a board with no cards
    When I launch the application
    When I add a card "persistent" to "To Do"
    When I quit the application
    When I launch the application
    Then the "To Do" column should contain "persistent"
